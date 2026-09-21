// ============================================================
// http_conn 业务路由：异步静态文件映射 + POST CGI 登录注册
//
// 文件系统调用（statx / openat）走 io_uring，不再占用载体线程；
// mmap 保留同步 —— 它本身不碰磁盘，真正读盘发生在缺页时，
// 配合 fadvise(WILLNEED) 把预读提前，冷文件首次发送也不会卡住 ring。
//
// MySQL 是阻塞 socket I/O，必须卸载到阻塞池，否则整个 ring 上的
// 所有协程都会被一个注册请求按死。
// ============================================================
#include "net/http_conn.h"
#include "db/user_cache.h"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "coro/awaiter.h"
#include "coro/offload.h"

namespace {

// 单个十六进制字符 → 数值，非法返回 -1
int hex_val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// application/x-www-form-urlencoded 解码：'+' → 空格，"%XX" → 字节。
// 浏览器对非 ASCII（比如中文用户名）一定会做百分号编码，不解码的话
// 存进库的就是 "%E5%BC%A0%E4%B8%89" 这种字面量。
std::string url_decode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
      continue;
    }
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex_val(s[i + 1]);
      const int lo = hex_val(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]); // 残缺/非法的转义序列原样保留，不吞字符
  }
  return out;
}

// 从 POST body 里取一个字段。
//
// 为什么不能直接 find("user=")：那样会匹配到 "username=" 的中间。
// 所以要求匹配位置是整个 body 的开头，或紧跟 '&'。
std::string form_field(const std::string &body, const std::string &key) {
  const std::string pat = key + "=";
  size_t pos = 0;
  while ((pos = body.find(pat, pos)) != std::string::npos) {
    const bool at_boundary = (pos == 0) || (body[pos - 1] == '&');
    if (at_boundary) {
      const size_t vs = pos + pat.size();
      size_t ve = body.find('&', vs);
      if (ve == std::string::npos)
        ve = body.size();
      return url_decode(body.substr(vs, ve - vs));
    }
    pos += pat.size();
  }
  return std::string();
}

} // namespace

// ===================== CGI 登录 / 注册 =====================
coro::Task<http_conn::HTTP_CODE>
http_conn::do_cgi_login(std::string name, std::string password,
                        char url_suffix) {
  m_real_file = m_opt.doc_root + "/" + m_url.substr(2);
  UserCache *cache = UserCache::getInstance();

  if (url_suffix == '3') {
    // 注册：先查内存缓存里有没有重名
    if (cache->user_exists(name)) {
      m_url = "/registerError.html";
      co_return REDIRECT;
    }

    // ★ 卸载到阻塞池。conn_pool 为空（未配置数据库）时 offload 的
    //   await_suspend 直接返回 false，rc 保持 -1，退化为注册失败。
    int rc = -1;
    co_await coro::offload(m_sched->blocking_pool(), [&] {
      MYSQL *db = nullptr;
      connectionRAII raii(&db, m_opt.conn_pool);
      if (db == nullptr)
        return;

      // 转义要用连接自身的字符集，所以只能在拿到 db 之后做 ——
      // 这也是拼 SQL 放在 lambda 里而不是外面的原因。
      // 不做转义的话用户名里一个单引号就能改写整条语句。
      std::vector<char> bn(name.size() * 2 + 1);
      std::vector<char> bp(password.size() * 2 + 1);
      const unsigned long ln = mysql_real_escape_string(
          db, bn.data(), name.c_str(), static_cast<unsigned long>(name.size()));
      const unsigned long lp =
          mysql_real_escape_string(db, bp.data(), password.c_str(),
                                   static_cast<unsigned long>(password.size()));

      const std::string sql =
          "INSERT INTO user(username, passwd) VALUES('" +
          std::string(bn.data(), ln) + "', '" + std::string(bp.data(), lp) +
          "')";
      rc = mysql_query(db, sql.c_str());
    });

    if (rc == 0) {
      cache->insert_user(name, password);
      m_url = "/log.html";
    } else {
      m_url = "/registerError.html";
    }
    co_return REDIRECT;
  }

  // 登录：UserCache 是纯内存 map，临界区纳秒级，直接在载体线程上判即可
  m_url = cache->validate_user(name, password) ? "/welcome.html"
                                                : "/logError.html";
  co_return REDIRECT;
}

// ===================== do_request =====================
coro::Task<http_conn::HTTP_CODE> http_conn::do_request() {
  m_real_file = m_opt.doc_root;

  // 获取URL最后一个'/'之后的首字符，用于路由判断
  const size_t last_slash = m_url.rfind('/');
  if (last_slash == std::string::npos)
    co_return BAD_REQUEST;
  const char url_suffix =
      (last_slash + 1 < m_url.length()) ? m_url[last_slash + 1] : '\0';

  // ===================== CGI登录/注册处理 =====================
  if (cgi == 1 && (url_suffix == '2' || url_suffix == '3')) {
    // 字段名以 static/*.html 里的表单为准：user / password。
    // 兜底再试一次 passwd —— 数据库列名、以及上游 TinyWebServer 的
    // 表单用的都是这个拼写，客户端有可能走的是老页面。
    const std::string name = form_field(m_post_data, "user");
    std::string password = form_field(m_post_data, "password");
    if (password.empty())
      password = form_field(m_post_data, "passwd");
    co_return co_await do_cgi_login(name, password, url_suffix);
  }

  // ===================== 页面路由映射 =====================
  if (url_suffix == '0')
    m_real_file = m_opt.doc_root + "/register.html";
  else if (url_suffix == '1')
    m_real_file = m_opt.doc_root + "/log.html";
  else if (url_suffix == '5')
    m_real_file = m_opt.doc_root + "/picture.html";
  else if (url_suffix == '6')
    m_real_file = m_opt.doc_root + "/video.html";
  else if (url_suffix == '7')
    m_real_file = m_opt.doc_root + "/fans.html";
  else
    m_real_file = m_opt.doc_root + m_url;

  // ===================== 异步 statx =====================
  // m_real_file 与 m_file_stat 都是成员，跨挂起存活没有问题
  const int sr = co_await coro::async_statx(
      AT_FDCWD, m_real_file.c_str(), 0, STATX_SIZE | STATX_MODE, &m_file_stat);
  if (sr < 0)
    co_return NO_RESOURCE;

  if (!(m_file_stat.stx_mode & S_IROTH))
    co_return FORBIDDEN_REQUEST;

  if (S_ISDIR(m_file_stat.stx_mode))
    co_return BAD_REQUEST;

  // ===================== 异步 openat =====================
  const int fd =
      co_await coro::async_openat(AT_FDCWD, m_real_file.c_str(), O_RDONLY, 0);
  if (fd < 0)
    co_return NO_RESOURCE;

  // ===================== 文件映射 =====================
  m_file_len = static_cast<size_t>(m_file_stat.stx_size);
  if (m_file_len > 0) {
    void *p = mmap(nullptr, m_file_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
      m_file_address = nullptr;
      m_file_len = 0;
      ::close(fd);
      co_return INTERNAL_ERROR;
    }
    m_file_address = static_cast<char *>(p);
    // 提前把文件读进 page cache，避免首次 send 时在载体线程上缺页
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
  }
  ::close(fd);
  co_return FILE_REQUEST;
}
