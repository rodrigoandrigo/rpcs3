#include "stdafx.h"
#include "dyn_lib.hpp"

#ifdef _WIN32
	#include <Windows.h>
	#include <winrt/base.h>
#else
	#include <dlfcn.h>
#endif

namespace utils
{
	dynamic_library::dynamic_library(const std::string& path)
	{
		load(path);
	}

	dynamic_library::~dynamic_library()
	{
		close();
	}

	bool dynamic_library::load(const std::string& path)
	{
#ifdef RPCS3_UWP
		m_handle = LoadPackagedLibrary(winrt::to_hstring(path).c_str(), 0);
#elif defined(_WIN32)
		m_handle = LoadLibraryA(path.c_str());
#else
		m_handle = dlopen(path.c_str(), RTLD_LAZY);
#endif
		return loaded();
	}

#ifdef _WIN32
	bool dynamic_library::load(const std::wstring& path)
	{
#ifdef RPCS3_UWP
		m_handle = LoadPackagedLibrary(path.c_str(), 0);
#else
		m_handle = LoadLibraryW(path.c_str());
#endif
		return loaded();
	}
#endif

	void dynamic_library::close()
	{
#ifdef _WIN32
		FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
		dlclose(m_handle);
#endif
		m_handle = nullptr;
	}

	void* dynamic_library::get_impl(const char* name) const
	{
#ifdef _WIN32
		return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name));
#else
		return dlsym(m_handle, name);
#endif
	}

	bool dynamic_library::loaded() const
	{
		return m_handle != nullptr;
	}

	dynamic_library::operator bool() const
	{
		return loaded();
	}

	void* get_proc_address(const char* lib, const char* name)
	{
#ifdef RPCS3_UWP
		// Optional system APIs are SDK imports, never unrestricted ntdll loading.
		if (std::strcmp(name, "SetThreadDescription") == 0 &&
			(_stricmp(lib, "Kernel32.dll") == 0 || _stricmp(lib, "KernelBase.dll") == 0))
			return reinterpret_cast<void*>(&SetThreadDescription);
		return nullptr;
#elif defined(_WIN32)
		return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(lib), name));
#else
		return dlsym(dlopen(lib, RTLD_NOLOAD), name);
#endif
	}
}
