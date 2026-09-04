#include "CTextBuffer.h"

CTextBuffer::CTextBuffer()
    : m_hFile(INVALID_HANDLE_VALUE)
    , m_hFileMapping(nullptr)
    , m_pMappedView(nullptr)
    , m_fileSize(0)
    , m_encoding(Encoding::Utf8)
{
}

CTextBuffer::~CTextBuffer()
{
    CloseFile();
}

BOOL CTextBuffer::OpenFile(LPCWSTR szPath)
{
    CloseFile();

    m_hFile = CreateFileW(szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    LARGE_INTEGER li{};
    if (!GetFileSizeEx(m_hFile, &li) || li.QuadPart < 0)
    {
        CloseFile();
        return FALSE;
    }
    m_fileSize = li.QuadPart;

    // 空文件：不建映射，表示空内容
    if (m_fileSize == 0)
    {
        m_path = szPath;
        m_encoding = Encoding::Utf8;
        return TRUE;
    }

    m_hFileMapping = CreateFileMappingW(m_hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m_hFileMapping)
    {
        CloseFile();
        return FALSE;
    }

    m_pMappedView = MapViewOfFile(m_hFileMapping, FILE_MAP_READ, 0, 0, 0);
    if (!m_pMappedView)
    {
        CloseFile();
        return FALSE;
    }

    // 编码检测：取开头 sample 字节（不超过 64KB）
    DWORD sample = static_cast<DWORD>(m_fileSize < 65536 ? m_fileSize : 65536);
    m_encoding = DetectEncoding(static_cast<const BYTE*>(m_pMappedView), sample);

    m_path = szPath;
    return TRUE;
}

void CTextBuffer::CloseFile()
{
    if (m_pMappedView)
    {
        UnmapViewOfFile(m_pMappedView);
        m_pMappedView = nullptr;
    }
    if (m_hFileMapping)
    {
        CloseHandle(m_hFileMapping);
        m_hFileMapping = nullptr;
    }
    if (m_hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }
    m_fileSize = 0;
    m_path.clear();
}

const BYTE* CTextBuffer::GetBasePtr() const
{
    return static_cast<const BYTE*>(m_pMappedView);
}

LONGLONG CTextBuffer::GetSize() const
{
    return m_fileSize;
}

Encoding CTextBuffer::GetEncoding() const
{
    return m_encoding;
}

LPCWSTR CTextBuffer::GetPath() const
{
    return m_path.c_str();
}