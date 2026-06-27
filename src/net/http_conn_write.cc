// ============================================================
// http_conn 响应拼接与发送：add_xxx、process_write、write、unmap
// ============================================================
#include "net/http_conn.h"
#include "net/http_const.h"
#include "net/socket_tool.h"
#include <cstdarg>
#include <sys/uio.h>

// ===================== unmap =====================
void http_conn::unmap() {
  if (m_file_address) {
    munmap(m_file_address, m_file_stat.st_size);
    m_file_address = 0;
  }
}

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

  LOG_INFO("request:%s", m_write_buf);

  return true;
}

// ===================== add_status_line =====================
bool http_conn::add_status_line(int status, const char *title) {
  return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// ===================== add_headers =====================
bool http_conn::add_headers(int content_len) {
  return add_content_length(content_len) && add_linger() && add_blank_line();
}

// ===================== add_content_length =====================
bool http_conn::add_content_length(int content_len) {
  return add_response("Content-Length:%d\r\n", content_len);
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
    if (m_file_stat.st_size != 0) {
      add_headers(m_file_stat.st_size);
      m_iv[0].iov_base = m_write_buf;
      m_iv[0].iov_len = m_write_idx;
      m_iv[1].iov_base = m_file_address;
      m_iv[1].iov_len = m_file_stat.st_size;
      m_iv_count = 2;
      bytes_to_send = m_write_idx + m_file_stat.st_size;
      return true;
    } else {
      const char *ok_string = "<html><body></body></html>";
      add_headers(strlen(ok_string));
      if (!add_content(ok_string))
        return false;
    }
  }
  default:
    return false;
  }
  m_iv[0].iov_base = m_write_buf;
  m_iv[0].iov_len = m_write_idx;
  m_iv_count = 1;
  bytes_to_send = m_write_idx;
  return true;
}

// ===================== write =====================
bool http_conn::write() {
  int temp = 0;

  if (bytes_to_send == 0) {
    modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
    init();
    return true;
  }

  while (1) {
    temp = writev(m_sockfd, m_iv, m_iv_count);

    if (temp < 0) {
      if (errno == EAGAIN) {
        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
        return true;
      }
      unmap();
      return false;
    }

    bytes_have_send += temp;
    bytes_to_send -= temp;
    if (bytes_have_send >= m_iv[0].iov_len) {
      m_iv[0].iov_len = 0;
      m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
      m_iv[1].iov_len = bytes_to_send;
    } else {
      m_iv[0].iov_base = m_write_buf + bytes_have_send;
      m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
    }

    if (bytes_to_send <= 0) {
      unmap();
      modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

      if (m_linger) {
        init();
        return true;
      } else {
        return false;
      }
    }
  }
}
