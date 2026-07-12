// ============================================================
// http_conn 业务路由：静态文件mmap、POST CGI登录注册数据库逻辑
// ============================================================
#include "net/http_conn.h"
#include "db/user_cache.h"
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ===================== do_request =====================
http_conn::HTTP_CODE http_conn::do_request() {
  m_real_file = doc_root;

  // 获取URL最后一个'/'之后的首字符，用于路由判断
  size_t last_slash = m_url.rfind('/');
  if (last_slash == std::string::npos)
    return BAD_REQUEST;
  char url_suffix =
      (last_slash + 1 < m_url.length()) ? m_url[last_slash + 1] : '\0';

  // ===================== CGI登录/注册处理 =====================
  if (cgi == 1 && (url_suffix == '2' || url_suffix == '3')) {

    std::string url_real = "/" + m_url.substr(2);
    m_real_file = doc_root + url_real;

    // 解析 POST body: "user=xxx&passwd=yyy"
    std::string post_data = m_post_data;
    size_t user_start = post_data.find("user=");
    size_t pass_start = post_data.find("&passwd=");
    std::string name, password;
    if (user_start != std::string::npos && pass_start != std::string::npos) {
      name     = post_data.substr(user_start + 5, pass_start - user_start - 5);
      password = post_data.substr(pass_start + 8);
    }

    UserCache *cache = UserCache::getInstance();

    if (url_suffix == '3') {
      // 如果是注册，先检测数据库中是否有重名的
      // 没有重名的，进行增加数据
      char *sql_insert = (char *)malloc(sizeof(char) * 200);
      strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
      strcat(sql_insert, "'");
      strcat(sql_insert, name.c_str());
      strcat(sql_insert, "', '");
      strcat(sql_insert, password.c_str());
      strcat(sql_insert, "')");

      if (!cache->user_exists(name)) {
        int res = mysql_query(mysql, sql_insert);
        cache->insert_user(name, password);

        if (!res)
          m_url = "/log.html";
        else
          m_url = "/registerError.html";
      } else
        m_url = "/registerError.html";
      free(sql_insert);
      return REDIRECT;
    } else if (url_suffix == '2') {
      // 如果是登录，直接判断
      // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
      if (cache->validate_user(name, password))
        m_url = "/welcome.html";
      else
        m_url = "/logError.html";
      return REDIRECT;
    }
  }

  // ===================== 页面路由映射 =====================
  if (url_suffix == '0')
    m_real_file = doc_root + "/register.html";
  else if (url_suffix == '1')
    m_real_file = doc_root + "/log.html";
  else if (url_suffix == '5')
    m_real_file = doc_root + "/picture.html";
  else if (url_suffix == '6')
    m_real_file = doc_root + "/video.html";
  else if (url_suffix == '7')
    m_real_file = doc_root + "/fans.html";
  else
    m_real_file = doc_root + m_url;

  // ===================== 文件映射 =====================
  if (stat(m_real_file.c_str(), &m_file_stat) < 0)
    return NO_RESOURCE;

  if (!(m_file_stat.st_mode & S_IROTH))
    return FORBIDDEN_REQUEST;

  if (S_ISDIR(m_file_stat.st_mode))
    return BAD_REQUEST;

  int fd = open(m_real_file.c_str(), O_RDONLY);
  m_file_address =
      (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  return FILE_REQUEST;
}
