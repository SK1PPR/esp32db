#pragma once
// engine.hpp — what each engine_*.cpp implements behind kv::put/get/del.
// The public wrappers in kv.cpp count the calls (op_counts()) and forward here.

#include "kv/kv.hpp"

namespace kv {

esp_err_t engine_put(Key key, const void *value, size_t len);
esp_err_t engine_get(Key key, void *out, size_t cap, size_t *out_len);
esp_err_t engine_del(Key key);

}  // namespace kv
