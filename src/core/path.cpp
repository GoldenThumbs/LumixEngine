#include "core/lux.h"
#include "core/path.h"

#include "core/crc32.h"
#include "core/path_utils.h"

#include <string>

namespace Lux
{
	Path::Path(const Path& rhs)
		: m_id(rhs.m_id)
	{
		uint32_t len = strlen(rhs.m_path);
		strcpy(m_path, rhs.m_path);
	}

	Path::Path(const char* path)
	{
		uint32_t len = strlen(path);
		PathUtils::normalize(path, m_path, len + 1);
		m_id = crc32(m_path);
	}

	Path::Path(uint32_t id, const char* path)
		: m_id(id)
	{
		uint32_t len = strlen(path);
		strcpy(m_path, path);
	}

	Path::~Path()
	{
	}
}