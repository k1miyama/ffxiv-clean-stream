#pragma once

namespace fcs::host {

struct StatusSink {
    void* context;
    void (*write)(void* context, const wchar_t* text);
};

inline void ReportStatus(StatusSink sink, const wchar_t* text) {
    if (sink.write) sink.write(sink.context, text);
}

} // namespace fcs::host
