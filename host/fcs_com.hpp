#pragma once

namespace fcs::host {

template <typename T>
void ReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

} // namespace fcs::host
