// Replaces Play!'s per-executable AppConfig. Preferences come from PS2GFX_CONFIG if set
// (e.g. <Config><Preference Name="renderer.opengl.resfactor" Type="integer" Value="2"/></Config>)
// and are written back there at exit; otherwise they live in memory only (/dev/null).
#include <cstdlib>
#include "AppConfig.h"

CAppConfig::CAppConfig()
    : CConfig(getenv("PS2GFX_CONFIG") ? getenv("PS2GFX_CONFIG") : "/dev/null")
{
}
