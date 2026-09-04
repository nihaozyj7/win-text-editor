#pragma once
#include <windows.h>
#include <string>
#include "Encoding.h"

// 数据层：内存映射文件（MMF 只读）+ 编码检测
// 保存时由 Piece Table 遍历重写；本类仅负责原始文件的映射与元信息
class CTextBuffer
{
public:
    CTextBuffer();
    ~CTextBuffer();

    // 打开文件并建立只读映射；返回 FALSE 表示失败（GetLastError 可查原因）
    BOOL OpenFile(LPCWSTR szPath);
    void CloseFile();

    // 映射视图基址与大小（只读，禁止写入）
    const BYTE* GetBasePtr() const;
    LONGLONG    GetSize() const;

    Encoding    GetEncoding() const;
    LPCWSTR     GetPath() const;

private:
    HANDLE    m_hFile;
    HANDLE    m_hFileMapping;
    LPVOID    m_pMappedView;
    LONGLONG  m_fileSize;
    Encoding  m_encoding;
    std::wstring m_path;
};