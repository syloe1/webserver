#ifndef HTTP_CONST_H
#define HTTP_CONST_H

// ============================================================
// HTTP响应状态码描述 & 错误页面模板 —— 仅头文件，无cpp
// ============================================================

// 200 OK
const char *const ok_200_title = "OK";

// 400 Bad Request
const char *const error_400_title = "Bad Request";
const char *const error_400_form =
    "Your request has bad syntax or is inherently impossible to staisfy.\n";

// 403 Forbidden
const char *const error_403_title = "Forbidden";
const char *const error_403_form =
    "You do not have permission to get file form this server.\n";

// 404 Not Found
const char *const error_404_title = "Not Found";
const char *const error_404_form =
    "The requested file was not found on this server.\n";

// 500 Internal Server Error
const char *const error_500_title = "Internal Error";
const char *const error_500_form =
    "There was an unusual problem serving the request file.\n";

#endif
