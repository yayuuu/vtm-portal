// SPDX-License-Identifier: MIT
#pragma once
#include <gio/gio.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace vtmportal {
namespace fs = std::filesystem;
struct VariantUnref { void operator()(GVariant* p) const { if (p) g_variant_unref(p); } };
using Variant = std::unique_ptr<GVariant, VariantUnref>;
Variant own(GVariant* value);

struct Config {
    std::vector<std::string> vtm_command{"vtm"}, browser_command{"carbonyl"}, picker_command{"elio"};
    std::vector<std::string> schemes{"http", "https", "file"};
    std::string session, browser_size="180:58", browser_title="Browser", picker_size="150:50", terminal_size="120:40";
    double launch_timeout=15, start_timeout=30;
};
Config configuration();
std::string environment(const char* name);
std::vector<std::string> executable(std::vector<std::string> argv);
std::string helper_path();
std::string lua_string(const std::string& value);
std::string encode_argv(const std::vector<std::string>& argv);
std::vector<std::string> decode_argv(const std::string& payload);
void exec_command(const std::vector<std::string>& argv);
void launch(std::vector<std::string> argv, const std::string& item,
            const std::string& title, const std::string& size, const Config& config);
std::string normalize_uri(const std::string& value, const Config& config);
std::string read_file(const fs::path& path, std::size_t max_size=1024*1024);
void write_file(const fs::path& path, const std::string& value);
Variant parse_variant(const std::string& text, const char* type);
std::string variant_string(GVariant* value);
Variant option(GVariant* options, const char* key, const char* type);
std::string string_option(GVariant* options, const char* key, const std::string& fallback="");
bool bool_option(GVariant* options, const char* key, bool fallback=false);
std::string byte_path(GVariant* value);
struct Request {
    std::string mode, title;
    Variant options;
};
Request read_request(const fs::path& job);
void write_response(const fs::path& job, unsigned response, GVariant* results);
bool filter_matches(const fs::path& path, GVariant* filter);
void valid_name(const std::string& name);
Variant translate(const std::vector<fs::path>& paths, GVariant* options,
                  const std::string& mode, const std::string& name, GVariant* extra);
int run_picker(const fs::path& job);
}
