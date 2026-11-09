// stb_vorbis (public domain / MIT, Sean Barrett): decodes the tavern music.
#pragma warning(push, 0)
#define STB_VORBIS_NO_PUSHDATA_API
extern "C" {
#include "../../extern/stb/stb_vorbis.c"
}
#pragma warning(pop)
