#pragma once

namespace fcs::host {

enum class StatusSeverity {
    Info,
    Error,
};

struct StatusSink {
    void* context;
    void (*write)(void* context, const wchar_t* text,
                  StatusSeverity severity);
};

inline void ReportStatus(StatusSink sink, const wchar_t* text) {
    if (sink.write) {
        sink.write(sink.context, text, StatusSeverity::Info);
    }
}

inline void ReportError(StatusSink sink, const wchar_t* text) {
    if (sink.write) {
        sink.write(sink.context, text, StatusSeverity::Error);
    }
}

} // namespace fcs::host
