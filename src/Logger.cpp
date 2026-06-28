#include "Logger.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <filesystem>
#include <memory>

namespace {
    std::shared_ptr<spdlog::logger> g_logger;
}

namespace redscope::log {

void Init(const wchar_t* logDir) {
    try {
        auto dir   = std::filesystem::path(logDir);
        auto path  = dir / L"REDscope-internal.log";
        auto path1 = dir / L"REDscope-internal.log.1";
        std::error_code ec;
        std::filesystem::remove(path1, ec);
        std::filesystem::rename(path, path1, ec);

        auto pathUtf8 = path.string();
        g_logger = spdlog::basic_logger_mt("redscope", pathUtf8, true);
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        g_logger->flush_on(spdlog::level::warn);
    } catch (...) {
    }
}

void Shutdown() {
    if (g_logger) {
        g_logger->flush();
        g_logger.reset();
    }
}

void Flush() { if (g_logger) g_logger->flush(); }

void Info(std::string_view msg)  { if (g_logger) g_logger->info(msg);  }
void Warn(std::string_view msg)  { if (g_logger) g_logger->warn(msg);  }
void Error(std::string_view msg) { if (g_logger) g_logger->error(msg); }

}
