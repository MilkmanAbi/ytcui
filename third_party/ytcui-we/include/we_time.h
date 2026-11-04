#pragma once
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
namespace ytwe {
inline double timestamp_seconds(const std::string &value) {
  std::istringstream input(value);
  std::string part;
  double total = 0;
  size_t count = 0;
  while (std::getline(input, part, ':')) {
    if (part.empty() ||
        part.find_first_not_of("0123456789.") != std::string::npos)
      throw std::runtime_error("invalid section timestamp");
    size_t consumed = 0;
    auto seconds = std::stod(part, &consumed);
    if (consumed != part.size() || (count && seconds >= 60) || ++count > 3)
      throw std::runtime_error("invalid section timestamp");
    total = total * 60 + seconds;
  }
  if (!count || value.back() == ':')
    throw std::runtime_error("invalid section timestamp");
  return total;
}
inline int64_t utc_timestamp(std::tm value) {
#ifdef _WIN32
  return _mkgmtime64(&value);
#else
  return timegm(&value);
#endif
}
inline int64_t
parse_time_text(std::string value,
                int64_t now = static_cast<int64_t>(std::time(nullptr))) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::smatch match;
  if (std::regex_search(value, match,
                        std::regex("([0-9]+)\\s*(second|minute|hour|day|week|"
                                   "month|year)s?\\s+ago"))) {
    int64_t count = std::stoll(match[1]);
    auto unit = match[2].str();
    if (unit == "second")
      return now - count;
    if (unit == "minute")
      return now - count * 60;
    if (unit == "hour")
      return now - count * 3600;
    if (unit == "day")
      return now - count * 86400;
    if (unit == "week")
      return now - count * 604800;
    auto time = static_cast<std::time_t>(now);
    std::tm date = *std::gmtime(&time);
    if (unit == "month")
      date.tm_mon -= static_cast<int>(count);
    else
      date.tm_year -= static_cast<int>(count);
    return utc_timestamp(date);
  }
  if (value.find("yesterday") != std::string::npos)
    return now - 86400;
  if (std::regex_search(value, match,
                        std::regex("([0-9]{4})-([0-9]{2})-([0-9]{2})(?:[t "
                                   "]([0-9]{2}):([0-9]{2}):([0-9]{2}))?"))) {
    std::tm date{};
    date.tm_year = std::stoi(match[1]) - 1900;
    date.tm_mon = std::stoi(match[2]) - 1;
    date.tm_mday = std::stoi(match[3]);
    if (match[4].matched) {
      date.tm_hour = std::stoi(match[4]);
      date.tm_min = std::stoi(match[5]);
      date.tm_sec = std::stoi(match[6]);
    }
    return utc_timestamp(date);
  }
  return -1;
}
inline std::string date_digits(std::string value) {
  std::string out;
  for (char c : value) {
    if (c >= '0' && c <= '9')
      out += c;
    if (out.size() == 8)
      break;
  }
  return out.size() == 8 ? out : "";
}
} // namespace ytwe
