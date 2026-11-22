#include "Platform/luduvo/context/LuduvoStudioContext.hpp"

#include "LSP/AtomicFile.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <libproc.h>
#include <sys/proc_info.h>
#endif
#endif

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
#include "LSP/Uri.hpp"
#include "LSP/Utils.hpp"

#include <charconv>
#include <chrono>
#endif

namespace luduvo
{
namespace
{
std::filesystem::path studioCapturePath(const ContextStore& store, const std::string& projectId)
{
    return std::filesystem::u8path(store.directory()) / "projects" / projectId / "world.studio.json";
}

#ifdef _WIN32
uint16_t studioLanguageServerPort()
{
    DWORD port = 0;
    DWORD size = sizeof(port);
    if (RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Luduvo\Luduvo Studio\editor)", L"lsp_port", RRF_RT_REG_DWORD, nullptr, &port, &size) ==
            ERROR_SUCCESS &&
        port <= UINT16_MAX)
        return static_cast<uint16_t>(port);
    return 6009;
}

bool ownsStudioPort(uint64_t pid, uint16_t port)
{
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<DWORD> buffer((size + sizeof(DWORD) - 1) / sizeof(DWORD));
    if (!size || GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return false;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index)
    {
        const auto& row = table->table[index];
        if (row.dwOwningPid == pid && row.dwLocalAddr == htonl(INADDR_LOOPBACK) && ntohs(static_cast<u_short>(row.dwLocalPort)) == port)
            return true;
    }
    return false;
}
#elif defined(__linux__)
uint16_t studioLanguageServerPort()
{
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const auto directory = config && *config ? std::filesystem::u8path(config)
                                             : home && *home ? std::filesystem::u8path(home) / ".config" : std::filesystem::path{};
    if (directory.empty())
        return 6009;
    std::ifstream input(directory / "Luduvo" / "Luduvo Studio.conf");
    bool editor = false;
    for (std::string line; std::getline(input, line);)
    {
        trim(line);
        if (!line.empty() && line.front() == '[')
            editor = line == "[editor]";
        const auto equals = line.find('=');
        if (!editor || equals == std::string::npos)
            continue;
        auto key = line.substr(0, equals);
        trim(key);
        if (key != "lsp_port")
            continue;
        auto value = line.substr(equals + 1);
        trim(value);
        uint16_t port = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
            return port;
    }
    return 6009;
}

bool ownsStudioPort(uint64_t pid, uint16_t port)
{
    const auto process = std::filesystem::path("/proc") / std::to_string(pid);
    std::ifstream input(process / "net/tcp");
    std::set<std::string> sockets;
    for (std::string line; std::getline(input, line);)
    {
        std::istringstream row(line);
        std::string slot, local, remote, state, queues, timers, retransmit, uid, timeout, inode;
        if (!(row >> slot >> local >> remote >> state >> queues >> timers >> retransmit >> uid >> timeout >> inode) ||
            state != "0A" || local.compare(0, 9, "0100007F:") != 0)
            continue;
        unsigned localPort = 0;
        const auto parsed = std::from_chars(local.data() + 9, local.data() + local.size(), localPort, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == local.data() + local.size() && localPort == port)
            sockets.insert("socket:[" + inode + "]");
    }
    if (sockets.empty())
        return false;
    std::error_code error;
    for (std::filesystem::directory_iterator fd(process / "fd", error), end; !error && fd != end; fd.increment(error))
    {
        std::error_code linkError;
        if (sockets.count(std::filesystem::read_symlink(fd->path(), linkError).string()))
            return true;
    }
    return false;
}

#elif defined(__APPLE__)
uint16_t studioLanguageServerPort()
{
    const auto value = CFPreferencesCopyAppValue(CFSTR("editor.lsp_port"), CFSTR("com.luduvo-dev.Luduvo Studio"));
    if (!value)
        return 6009;

    int port = -1;
    const bool valid = CFGetTypeID(value) == CFNumberGetTypeID() &&
                       CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &port) &&
                       port >= 0 && port <= UINT16_MAX;
    CFRelease(value);
    return valid ? static_cast<uint16_t>(port) : 6009;
}

bool ownsStudioPort(uint64_t pid, uint16_t port)
{
    const auto process = static_cast<int>(pid);
    const int size = proc_pidinfo(process, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (size < static_cast<int>(sizeof(struct proc_fdinfo)))
        return false;
    std::vector<struct proc_fdinfo> descriptors(size / PROC_PIDLISTFD_SIZE);
    const auto capacity = static_cast<int>(descriptors.size() * sizeof(struct proc_fdinfo));
    const int bytes = proc_pidinfo(process, PROC_PIDLISTFDS, 0, descriptors.data(), capacity);
    if (bytes <= 0)
        return false;
    for (size_t index = 0; index < static_cast<size_t>(bytes) / PROC_PIDLISTFD_SIZE && index < descriptors.size(); ++index)
    {
        const auto& descriptor = descriptors[index];
        struct socket_fdinfo socketInfo{};
        if (descriptor.proc_fdtype != PROX_FDTYPE_SOCKET ||
            proc_pidfdinfo(process, descriptor.proc_fd, PROC_PIDFDSOCKETINFO, &socketInfo, sizeof(socketInfo)) != sizeof(socketInfo))
            continue;
        const auto& socket = socketInfo.psi;
        if (socket.soi_family != AF_INET || socket.soi_kind != SOCKINFO_TCP)
            continue;
        const auto& tcp = socket.soi_proto.pri_tcp;
        if (tcp.tcpsi_state == TSI_S_LISTEN && tcp.tcpsi_ini.insi_laddr.ina_46.i46a_addr4.s_addr == htonl(INADDR_LOOPBACK) &&
            ntohs(static_cast<uint16_t>(tcp.tcpsi_ini.insi_lport)) == port)
            return true;
    }
    return false;
}
#endif

#if defined(__linux__) || defined(__APPLE__)
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
#endif

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
class StudioConnection
{
    SOCKET socketHandle = INVALID_SOCKET;
#ifdef _WIN32
    bool initialized = false;
#endif
    int requestId = 0;
    std::string input;
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    bool fill()
    {
        if (std::chrono::steady_clock::now() >= deadline || input.size() >= 1024 * 1024)
            return false;
        char bytes[4096];
        const int received = recv(socketHandle, bytes, sizeof(bytes), 0);
        if (received <= 0)
            return false;
        input.append(bytes, received);
        return true;
    }

public:
    StudioConnection(uint64_t pid, uint16_t port)
    {
#ifdef _WIN32
        WSADATA data{};
        initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!initialized)
            return;
#endif
        if (!ownsStudioPort(pid, port))
            return;
#ifdef __linux__
        socketHandle = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
        socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
        if (socketHandle == INVALID_SOCKET)
            return;
#ifdef _WIN32
        const DWORD timeout = 750;
#else
        const timeval timeout{0, 750000};
#endif
        const int noDelay = 1;
#ifdef __APPLE__
        const int noSignal = 1;
        if (fcntl(socketHandle, F_SETFD, FD_CLOEXEC) == -1 ||
            setsockopt(socketHandle, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal)) == -1)
        {
            closeSocket();
            return;
        }
#endif
        setsockopt(socketHandle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!setNonblocking(true))
        {
            closeSocket();
            return;
        }
        if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        {
            // A loopback handshake may still be in progress.
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socketHandle, &writable);
            timeval wait{0, 80000};
            int error = 0;
#ifdef _WIN32
            int length = sizeof(error);
            const int nfds = 0;
#else
            socklen_t length = sizeof(error);
            const int nfds = socketHandle + 1;
#endif
            if (select(nfds, nullptr, &writable, nullptr, &wait) != 1 ||
                getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) || error)
            {
                closeSocket();
                return;
            }
        }
        if (!setNonblocking(false) || !ownsStudioPort(pid, port))
            closeSocket();
    }

    ~StudioConnection()
    {
        if (socketHandle != INVALID_SOCKET)
            closeSocket();
#ifdef _WIN32
        if (initialized)
            WSACleanup();
#endif
    }

    bool sendMessage(const char* method, nlohmann::json params, int id = 0)
    {
        if (socketHandle == INVALID_SOCKET)
            return false;
        nlohmann::json message{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
        if (id)
            message["id"] = id;
        const auto body = message.dump();
        const auto frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < frame.size() && std::chrono::steady_clock::now() < deadline)
        {
#ifdef __linux__
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto count = send(socketHandle, frame.data() + sent, static_cast<int>(frame.size() - sent), flags);
            if (count <= 0)
                return false;
            sent += count;
        }
        return sent == frame.size();
    }

    std::optional<nlohmann::json> request(const char* method, nlohmann::json params)
    {
        const int id = ++requestId;
        if (!sendMessage(method, std::move(params), id))
            return std::nullopt;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto header = input.find("\r\n\r\n");
            while (header == std::string::npos)
            {
                if (!fill())
                    return std::nullopt;
                header = input.find("\r\n\r\n");
            }
            constexpr std::string_view prefix = "Content-Length:";
            if (input.compare(0, prefix.size(), prefix) != 0)
                return std::nullopt;
            const auto start = input.find_first_not_of(' ', prefix.size());
            if (start == std::string::npos || start >= header)
                return std::nullopt;
            size_t length = 0;
            const auto parsed = std::from_chars(input.data() + start, input.data() + header, length);
            if (parsed.ec != std::errc{} || length > 1024 * 1024)
                return std::nullopt;
            const auto body = header + 4;
            while (input.size() < body + length)
                if (!fill())
                    return std::nullopt;
            auto reply = nlohmann::json::parse(input.substr(body, length), nullptr, false);
            input.erase(0, body + length);
            if (reply.is_object() && reply.contains("id") && reply["id"] == id)
            {
                if (reply.contains("result"))
                    return reply["result"];
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

private:
    bool setNonblocking(bool enabled)
    {
#ifdef _WIN32
        u_long mode = enabled;
        return ioctlsocket(socketHandle, FIONBIO, &mode) == 0;
#else
        int mode = enabled;
        return socketHandle < FD_SETSIZE && ioctl(socketHandle, FIONBIO, &mode) == 0;
#endif
    }

    void closeSocket()
    {
#ifdef _WIN32
        closesocket(socketHandle);
#else
        close(socketHandle);
#endif
        socketHandle = INVALID_SOCKET;
    }
};
#endif
}

std::optional<StudioHandles> captureStudioHandles(const ContextProject& project, const ProcessIdentity& studio)
{
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    const auto port = studioLanguageServerPort();
    if (!port)
        return std::nullopt;
    StudioConnection connection(studio.processId, port);
    const auto init = connection.request("initialize", {{"rootUri", Uri::file(project.directory).toString()}, {"capabilities", nlohmann::json::object()}});
    if (!init || !init->is_object() || !connection.sendMessage("initialized", nlohmann::json::object()))
        return std::nullopt;
    StudioHandles handles;
    for (const auto& script : project.scripts)
    {
        const auto relative = std::filesystem::u8path(script).lexically_relative(std::filesystem::u8path(project.directory));
        if (relative.empty() || *relative.begin() != "scripts")
            continue;
#ifdef _WIN32
        // The catalog folds Windows paths. Studio resolves attachments by their saved script spelling.
        const auto path = std::filesystem::u8path(script);
        const HANDLE found = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (found == INVALID_HANDLE_VALUE)
            continue;
        std::wstring actualPath(32768, L'\0');
        const auto length = GetFinalPathNameByHandleW(found, actualPath.data(), static_cast<DWORD>(actualPath.size()), FILE_NAME_NORMALIZED);
        CloseHandle(found);
        if (length <= 4 || length >= actualPath.size() || actualPath.compare(0, 4, L"\\\\?\\") != 0)
            continue;
        const auto spelling = std::filesystem::path(actualPath.substr(4, length - 4)).generic_u8string();
        auto uri = Uri::file(spelling).toString();
        // Studio expects the drive colon as a literal separator, as in native file URLs.
        if (uri.compare(9, 3, "%3A") == 0)
            uri.replace(9, 3, ":");
        if (uri.size() > 9 && spelling.size() > 1 && spelling[1] == ':')
            uri[8] = spelling[0];
#else
        auto uri = Uri::file(script).toString();
#endif
        const nlohmann::json document{{"uri", uri}};
        if (!connection.sendMessage("textDocument/didOpen",
                {{"textDocument", {{"uri", uri}, {"version", 1}, {"languageId", "luau"}, {"text", "local probe = handles.__lsp_probe\n"}}}}))
            break;
        const auto result = connection.request("textDocument/completion", {{"textDocument", document}, {"position", {{"line", 0}, {"character", 22}}}});
        connection.sendMessage("textDocument/didClose", {{"textDocument", document}});
        if (!result || !result->is_object() || result->find("isIncomplete") == result->end() || (*result)["isIncomplete"] != false ||
            !result->contains("items") || !(*result)["items"].is_array())
            break;
        auto& names = handles[normalizeContextPath(script)];
        for (const auto& item : (*result)["items"])
            if (item.is_object() && item.contains("label") && item["label"].is_string())
                names.push_back(item["label"].get<std::string>());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }
    if (handles.empty() || !isProcessAlive(studio) || !ownsStudioPort(studio.processId, port))
        return std::nullopt;
    return handles;
#else
    (void)project;
    (void)studio;
    return std::nullopt;
#endif
}

std::optional<StudioCapture> loadStudioCapture(const ContextStore& store, const ContextProject& project)
{
    const auto data = readSharedFile(studioCapturePath(store, project.id).generic_u8string());
    if (!data)
        return std::nullopt;
    try
    {
        const auto capture = nlohmann::json::parse(*data);
        if (capture.at("directory") != project.directory)
            return std::nullopt;
        const auto owner = readProcessIdentity(store.ownerPath());
        const ProcessIdentity studio{capture.at("pid").get<uint64_t>(), capture.at("start").get<uint64_t>()};
        const bool active = capture.value("active", true) && owner && capture.at("helper") == owner->startToken &&
                            capture.at("helperPid") == owner->processId && isProcessAlive(*owner) && isProcessAlive(studio);
        return StudioCapture{capture.at("snapshot").get<WorldDumpSnapshot>(), active};
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

void publishStudioHandles(const ContextStore& store, const ContextProject& project, const ProcessIdentity& studio, const StudioHandles& handles)
{
    const auto helper = currentProcessIdentity();
    if (!helper)
        return;
    const auto path = studioCapturePath(store, project.id);
    WorldDumpSnapshot snapshot;
    snapshot.scriptHandles = handles;
    nlohmann::json capture{{"helper", helper->startToken}, {"helperPid", helper->processId}, {"pid", studio.processId},
        {"start", studio.startToken}, {"directory", project.directory}, {"active", true}, {"snapshot", snapshot}};
    if (const auto data = readSharedFile(path.generic_u8string()))
    {
        try
        {
            auto previous = nlohmann::json::parse(*data);
            const auto timestamp = previous.at("snapshot").at("capture").at("capturedAtUnixMs");
            previous["snapshot"]["capture"]["capturedAtUnixMs"] = 0;
            if (timestamp.is_number_integer() && timestamp.get<int64_t>() > 0 && previous == capture)
                return;
        }
        catch (const nlohmann::json::exception&)
        {
        }
    }
    snapshot.capturedAtUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    capture["snapshot"] = snapshot;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!error)
        writeFileAtomically(path.generic_u8string(), capture.dump());
}

void clearStudioHandles(const ContextStore& store, const std::string& projectId)
{
    const auto path = studioCapturePath(store, projectId).generic_u8string();
    const auto data = readSharedFile(path);
    if (!data)
        return;
    try
    {
        auto capture = nlohmann::json::parse(*data);
        if (capture.value("active", true))
        {
            capture["active"] = false;
            writeFileAtomically(path, capture.dump());
        }
    }
    catch (const nlohmann::json::exception&)
    {
    }
}
}
