#pragma once
#include <windows.h>
#include <string>
#include <stdexcept>
// Redirect HKCU inside this test process. Never touch an actual Vizu installation's preferences.
struct IsolatedRegistry {
    HKEY key = nullptr;
    std::wstring path;
    IsolatedRegistry() {
        GUID id; wchar_t text[40] = {};
        if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("test registry GUID");
        StringFromGUID2(id,text,40);
        path=std::wstring(L"Software\\Vizu-test-")+text;
        if(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr)!=ERROR_SUCCESS) throw std::runtime_error("isolated test registry");
        if(RegOverridePredefKey(HKEY_CURRENT_USER,key)!=ERROR_SUCCESS) {RegCloseKey(key);key=nullptr;RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str());throw std::runtime_error("registry redirect");}
    }
    ~IsolatedRegistry() {if(key){RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);RegCloseKey(key);RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str());}}
};
