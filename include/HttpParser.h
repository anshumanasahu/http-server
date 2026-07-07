#pragma once
#include <string>
#include "Request.h"
#include "Connection.h"

class HttpParser {
public:
    // Parses as much as possible from conn.read_buf.
    // Returns INCOMPLETE if more data is needed, COMPLETE if a full request is parsed, ERROR if malformed.
    static ParseResult parse_incremental(Connection& conn);
    static std::string urlDecode(const std::string& src);
};
