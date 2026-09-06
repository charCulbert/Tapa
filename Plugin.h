#pragma once

#include <clap/clap.h>

namespace tapa
{

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace tapa
