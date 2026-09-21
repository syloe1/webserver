// ============================================================
// http_conn 读解析逻辑：HTTP 报文状态机
//
// 状态机本身与回调式版本完全一致 —— 这正是协程的价值所在：
// "数据不够"（NO_REQUEST）天然映射成一次 co_await，不需要把状态
// 拆成回调。只要保证 m_checked_idx / m_start_line / m_read_idx 这些
// 游标在跨挂起时保持正确，暂停再继续就是透明的。
// ============================================================
#include "net/http_conn.h"
#include <cstring>
#include <strings.h>

// ===================== parse_line =====================
// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line() {
  char temp;
  for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
    temp = m_read_buf[m_checked_idx];
    if (temp == '\r') {
      if ((m_checked_idx + 1) == m_read_idx)
        return LINE_OPEN;
      else if (m_read_buf[m_checked_idx + 1] == '\n') {
        m_read_buf[m_checked_idx++] = '\0';
        m_read_buf[m_checked_idx++] = '\0';
        return LINE_OK;
      }
      return LINE_BAD;
    } else if (temp == '\n') {
      if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
        m_read_buf[m_checked_idx - 1] = '\0';
        m_read_buf[m_checked_idx++] = '\0';
        return LINE_OK;
      }
      return LINE_BAD;
    }
  }
  return LINE_OPEN;
}

// ===================== parse_request_line =====================
// 解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(const char *text) {
  // 定位 URL 起始位置（method 之后的空格）
  const char *url_start = strpbrk(text, " \t");
  if (!url_start) {
    return BAD_REQUEST;
  }

  // 提取 HTTP 方法
  std::string method_str(text, url_start - text);
  if (strcasecmp(method_str.c_str(), "GET") == 0)
    m_method = GET;
  else if (strcasecmp(method_str.c_str(), "POST") == 0) {
    m_method = POST;
    cgi = 1;
  } else
    return BAD_REQUEST;

  // 跳过空白定位到 URL
  url_start += strspn(url_start, " \t");

  // 定位 HTTP 版本号（URL 之后的空格）
  const char *version_start = strpbrk(url_start, " \t");
  if (!version_start)
    return BAD_REQUEST;

  // 提取 URL
  m_url = std::string(url_start, version_start - url_start);

  // 跳过空白定位到版本号
  version_start += strspn(version_start, " \t");
  m_version = std::string(version_start);

  if (strcasecmp(m_version.c_str(), "HTTP/1.1") != 0 &&
      strcasecmp(m_version.c_str(), "HTTP/1.0") != 0)
    return BAD_REQUEST;

  // 去除 http:// 或 https:// 前缀
  if (m_url.compare(0, 7, "http://") == 0) {
    size_t pos = m_url.find('/', 7);
    if (pos != std::string::npos)
      m_url = m_url.substr(pos);
    else
      return BAD_REQUEST;
  } else if (m_url.compare(0, 8, "https://") == 0) {
    size_t pos = m_url.find('/', 8);
    if (pos != std::string::npos)
      m_url = m_url.substr(pos);
    else
      return BAD_REQUEST;
  }

  if (m_url.empty() || m_url[0] != '/')
    return BAD_REQUEST;

  // 当url为/时，显示判断界面
  if (m_url == "/")
    m_url = "/judge.html";

  m_check_state = CHECK_STATE_HEADER;
  return NO_REQUEST;
}

// ===================== parse_headers =====================
// 解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(const char *text) {
  if (text[0] == '\0') {
    if (m_content_length != 0) {
      m_check_state = CHECK_STATE_CONTENT;
      return NO_REQUEST;
    }
    // 无请求体：空行之后就是下一个请求的起点。
    // parse_line 已把 m_checked_idx 推过这一行的 CRLF。
    m_parse_consumed = m_checked_idx;
    return GET_REQUEST;
  } else if (strncasecmp(text, "Connection:", 11) == 0) {
    text += 11;
    text += strspn(text, " \t");
    if (strcasecmp(text, "keep-alive") == 0) {
      m_linger = true;
    }
  } else if (strncasecmp(text, "Content-length:", 15) == 0) {
    text += 15;
    text += strspn(text, " \t");
    m_content_length = atol(text);
  } else if (strncasecmp(text, "Host:", 5) == 0) {
    text += 5;
    text += strspn(text, " \t");
    m_host = std::string(text);
  }
  return NO_REQUEST;
}

// ===================== parse_content =====================
// 判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(const char *text) {
  if (m_read_idx >= (m_content_length + m_checked_idx)) {
    // POST请求中最后为输入的用户名和密码
    m_post_data = std::string(text, m_content_length);
    // 进入 CONTENT 状态时 m_start_line 与 m_checked_idx 都停在 body 起点
    m_parse_consumed = m_checked_idx + m_content_length;
    return GET_REQUEST;
  }
  return NO_REQUEST;
}

// ===================== process_read =====================
// 完整HTTP报文解析状态机主循环。
//
// 与回调版唯一的差别：解析完毕时返回 GET_REQUEST，而不是就地调用
// do_request()。因为 do_request() 现在是协程（要异步 statx/openat/
// 卸载 DB），调用点必须在协程里，所以把它上提到 run()。
// 这样 process_read 保持同步，热路径零协程开销。
http_conn::HTTP_CODE http_conn::process_read() {
  LINE_STATUS line_status = LINE_OK;
  HTTP_CODE ret = NO_REQUEST;
  char *text = 0;

  while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
         ((line_status = parse_line()) == LINE_OK)) {
    text = get_line();
    m_start_line = m_checked_idx;
    switch (m_check_state) {
    case CHECK_STATE_REQUESTLINE: {
      ret = parse_request_line(text);
      if (ret == BAD_REQUEST)
        return BAD_REQUEST;
      break;
    }
    case CHECK_STATE_HEADER: {
      ret = parse_headers(text);
      if (ret == BAD_REQUEST)
        return BAD_REQUEST;
      else if (ret == GET_REQUEST)
        return GET_REQUEST;
      break;
    }
    case CHECK_STATE_CONTENT: {
      ret = parse_content(text);
      if (ret == GET_REQUEST)
        return GET_REQUEST;
      line_status = LINE_OPEN;
      break;
    }
    default:
      return INTERNAL_ERROR;
    }
  }
  return NO_REQUEST;
}
