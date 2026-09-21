// ============================================================
// http_conn 响应拼接：add_xxx、process_write
//
// 与回调版的差别：不再预先把 m_iv[] 拼好等 on_send_cqe 原地改，
// 而是只记下响应头长度和文件体长度，由 send_all() 每轮按已发字节
// 重新构造 iovec。
// ============================================================
#include "net/http_conn.h"
#include "net/http_const.h"

#include <cstdarg>
#include <cstring>
#include <sys/uio.h>

// ===================== add_response =====================
bool http_conn::add_response(const char *format, ...) {
  if (m_write_idx >= WRITE_BUFFER_SIZE)
    return false;
  va_list arg_list;
  va_start(arg_list, format);
  int len = vsnprintf(m_write_buf + m_write_idx,
                      WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
  if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
    va_end(arg_list);
    return false;
  }
  m_write_idx += len;
  va_end(arg_list);
  return true;
}

// ===================== add_status_line =====================
bool http_conn::add_status_line(int status, const char *title) {
  return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// ===================== add_headers =====================
bool http_conn::add_headers(long long content_len) {
  return add_content_length(content_len) && add_linger() && add_blank_line();
}

// ===================== add_content_length =====================
bool http_conn::add_content_length(long long content_len) {
  return add_response("Content-Length:%lld\r\n", content_len);
}

// ===================== add_content_type =====================
bool http_conn::add_content_type() {
  return add_response("Content-Type:%s\r\n", "text/html");
}

// ===================== add_linger =====================
bool http_conn::add_linger() {
  return add_response("Connection:%s\r\n",
                      (m_linger == true) ? "keep-alive" : "close");
}

// ===================== add_blank_line =====================
bool http_conn::add_blank_line() { return add_response("%s", "\r\n"); }

// ===================== add_content =====================
bool http_conn::add_content(const char *content) {
  return add_response("%s", content);
}

// ===================== process_write =====================
bool http_conn::process_write(HTTP_CODE ret) {
  switch (ret) {
  case INTERNAL_ERROR: {
    add_status_line(500, error_500_title);
    add_headers(strlen(error_500_form));
    if (!add_content(error_500_form))
      return false;
    break;
  }
  case BAD_REQUEST: {
    // 400 就该回 400 —— 原实现在这里错发了 404 的页面
    add_status_line(400, error_400_title);
    add_headers(strlen(error_400_form));
    if (!add_content(error_400_form))
      return false;
    break;
  }
  case NO_RESOURCE: {
    add_status_line(404, error_404_title);
    add_headers(strlen(error_404_form));
    if (!add_content(error_404_form))
      return false;
    break;
  }
  case FORBIDDEN_REQUEST: {
    add_status_line(403, error_403_title);
    add_headers(strlen(error_403_form));
    if (!add_content(error_403_form))
      return false;
    break;
  }
  case FILE_REQUEST: {
    add_status_line(200, ok_200_title);
    if (m_file_stat.stx_size != 0) {
      add_headers(static_cast<long long>(m_file_stat.stx_size));
      m_file_len = static_cast<size_t>(m_file_stat.stx_size);
      return true;
    }
    const char *ok_string = "<html><body></body></html>";
    add_headers(strlen(ok_string));
    if (!add_content(ok_string))
      return false;
    break;
  }
  case REDIRECT: {
    add_status_line(302, "Found");
    add_response("Location:%s\r\n", m_url.c_str());
    add_headers(0);
    break;
  }
  default:
    return false;
  }
  // 非文件响应：只有响应头，没有 body 部分
  m_file_len = 0;
  return true;
}
