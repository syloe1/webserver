// ============================================================
// http_conn 读解析逻辑：非阻塞读、HTTP报文状态机解析
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

// ===================== read_once =====================
// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once() {
  if (m_read_idx >= READ_BUFFER_SIZE) {
    return false;
  }
  int bytes_read = 0;

  // LT读取数据
  if (0 == m_TRIGMode) {
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                      READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;

    if (bytes_read <= 0) {
      return false;
    }

    return true;
  }
  // ET读数据
  else {
    while (true) {
      bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                        READ_BUFFER_SIZE - m_read_idx, 0);
      if (bytes_read == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        return false;
      } else if (bytes_read == 0) {
        return false;
      }
      m_read_idx += bytes_read;
    }
    return true;
  }
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

  if (strcasecmp(m_version.c_str(), "HTTP/1.1") != 0)
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
  } else {
    LOG_INFO("oop!unknow header: %s", text);
  }
  return NO_REQUEST;
}

// ===================== parse_content =====================
// 判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(const char *text) {
  if (m_read_idx >= (m_content_length + m_checked_idx)) {
    // POST请求中最后为输入的用户名和密码
    m_post_data = std::string(text, m_content_length);
    return GET_REQUEST;
  }
  return NO_REQUEST;
}

// ===================== process_read =====================
// 完整HTTP报文解析状态机主循环
http_conn::HTTP_CODE http_conn::process_read() {
  LINE_STATUS line_status = LINE_OK;
  HTTP_CODE ret = NO_REQUEST;
  char *text = 0;

  while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
         ((line_status = parse_line()) == LINE_OK)) {
    text = get_line();
    m_start_line = m_checked_idx;
    LOG_INFO("%s", text);
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
      else if (ret == GET_REQUEST) {
        return do_request();
      }
      break;
    }
    case CHECK_STATE_CONTENT: {
      ret = parse_content(text);
      if (ret == GET_REQUEST)
        return do_request();
      line_status = LINE_OPEN;
      break;
    }
    default:
      return INTERNAL_ERROR;
    }
  }
  return NO_REQUEST;
}
