#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

// Opt-in visual QA only. Captures the actual window, excluding overlapping applications.
inline void CaptureWindow(HWND window, const std::filesystem::path& path, UINT flags=2)
{
    ShowWindow(window, SW_SHOWNOACTIVATE);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
    RECT r{}; GetWindowRect(window,&r);
    HDC dc=GetDC(window), buffer=CreateCompatibleDC(dc);
    HBITMAP bitmap=CreateCompatibleBitmap(dc,r.right-r.left,r.bottom-r.top);
    auto old=SelectObject(buffer,bitmap);
    const bool captured=PrintWindow(window,buffer,flags)!=FALSE;
    SelectObject(buffer,old);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=r.right-r.left; info.bmiHeader.biHeight=-(r.bottom-r.top);
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    std::vector<char> pixels(static_cast<std::size_t>(r.right-r.left)*(r.bottom-r.top)*4);
    const bool read=GetDIBits(buffer,bitmap,0,r.bottom-r.top,pixels.data(),&info,DIB_RGB_COLORS)!=0;
    DeleteObject(bitmap); DeleteDC(buffer); ReleaseDC(window,dc);
    if (!captured || !read) throw std::runtime_error("Screenshot capture failed");
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<char*>(&header),sizeof(header));
    file.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));
    file.write(pixels.data(),static_cast<std::streamsize>(pixels.size()));
    if (!file) throw std::runtime_error("Screenshot write failed");
}
