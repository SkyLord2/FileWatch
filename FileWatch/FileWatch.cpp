#include <node.h>
#include <uv.h>
#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <map>
#include <set>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <queue>
#include <restartmanager.h>

#pragma comment(lib, "Rstrtmgr.lib")

#define UNKUOWN				L"unknown"
#define UNKUOWN_NO_ORIGIN	L"unknown_no_original_file_found"
#define UNKUOWN_MAYBE_DEL	L"unknown_maybe_delete"

static std::set<std::wstring> target_extensions = {
	L".txt", L".doc", L".docx", L".pdf", L".xls", L".xlsx", L".ppt", L".pptx"
};

v8::Isolate* isolate = NULL;
v8::Local<v8::Function> logCallback;
v8::Local<v8::Function> fileCallback;

static uv_async_t async_log_handle;
// 存储日志信息 (需要线程安全)
struct FileMessage {
	std::wstring message;
	std::wstring type;
	std::wstring path;
	int messageType;
	// 可能还需要一个类型指示器 (Info/Error)
};

std::mutex log_mutex;
std::queue<FileMessage> log_queue;

std::string WcharToUtf8(const wchar_t* wstr) {
	if (wstr == nullptr) return "";
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
	if (size_needed == 0) return "";

	std::string result(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], size_needed, nullptr, nullptr);
	// 移除末尾的 null 终止符
	if (!result.empty() && result[result.size() - 1] == '\0') {
		result.pop_back();
	}
	return result;
}

std::wstring Utf8ToWstring(const std::string& utf8Str) {
	if (utf8Str.empty()) return L"";

	int wchars_count = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
	if (wchars_count == 0) return L"";

	std::vector<wchar_t> wchars(wchars_count);
	MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, wchars.data(), wchars_count);

	// 移除末尾的 null 终止符
	if (!wchars.empty() && wchars[wchars.size() - 1] == L'\0') {
		wchars.pop_back();
	}

	return std::wstring(wchars.begin(), wchars.end());
}

static void LogBase(const std::wstring& info) {
	if (isolate == NULL)
	{
		std::wcerr << L"isolate is null" << std::endl;
		return;
	}
	// 声明 HandleScope，确保局部 V8 对象的安全创建
	v8::HandleScope handle_scope(isolate);

	std::string logStr = WcharToUtf8(info.c_str());
	v8::Local<v8::Value> argv[1] = {
		v8::String::NewFromUtf8(isolate, logStr.c_str()).ToLocalChecked()
	};
	if (logCallback->IsNull() || logCallback->IsUndefined())
	{
		std::wcerr << L"logCallback is null" << std::endl;
		return;
	}
	if (isolate == NULL)
	{
		std::wcerr << L"isolate is null" << std::endl;
		return;
	}
	logCallback->Call(isolate->GetCurrentContext(),
		Null(isolate),
		1, argv).ToLocalChecked();
}

static void LogFunc(const std::wstring& info) {
	std::wcout << info << std::endl;
	// 1. 将日志信息放入线程安全队列
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		log_queue.push({ info, L"", L"", 0 });
	}

	// 2. 触发 Libuv 事件，通知主线程执行 AsyncLogCallback
	// 这是安全的，因为它只发送一个信号，不涉及 V8 对象。
	uv_async_send(&async_log_handle);
}

void LogError(const std::wstring& error) {
	std::wstring logInfo = L"[file watch error] " + error;
	LogFunc(logInfo);
}

void LogInfo(const std::wstring& info) {
	std::wstring logInfo = L"[file watch info] " + info;
	LogFunc(logInfo);
}

static void asyncReport(const std::wstring& type, const std::wstring& path) {
	std::wcout << path << std::endl;
	// 1. 将日志信息放入线程安全队列
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		log_queue.push({ L"", type, path, 1});
	}

	// 2. 触发 Libuv 事件，通知主线程执行 AsyncLogCallback
	// 这是安全的，因为它只发送一个信号，不涉及 V8 对象。
	uv_async_send(&async_log_handle);
}

static void reportBase(const std::wstring& type, const std::wstring& path) {
	if (isolate == NULL)
	{
		std::wcerr << L"isolate is null" << std::endl;
		return;
	}
	// 声明 HandleScope，确保局部 V8 对象的安全创建
	v8::HandleScope handle_scope(isolate);

	std::string typeStr = WcharToUtf8(type.c_str());
	std::string pathStr = WcharToUtf8(path.c_str());
	v8::Local<v8::Value> argv[2] = {
		v8::String::NewFromUtf8(isolate, typeStr.c_str()).ToLocalChecked(),
		v8::String::NewFromUtf8(isolate, pathStr.c_str()).ToLocalChecked()
	};
	if (fileCallback->IsNull() || fileCallback->IsUndefined())
	{
		std::wcerr << L"fileCallback is null" << std::endl;
		return;
	}
	if (isolate == NULL)
	{
		std::wcerr << L"isolate is null" << std::endl;
		return;
	}
	fileCallback->Call(isolate->GetCurrentContext(),
		Null(isolate),
		2, argv).ToLocalChecked();
}

// 异步回调：在 Node.js 主线程上执行
static void AsyncLogCallback(uv_async_t* handle) {
	// 必须在这里创建 HandleScope 
	v8::HandleScope handle_scope(isolate);

	// 从队列中取出所有等待的日志消息
	std::lock_guard<std::mutex> lock(log_mutex);

	while (!log_queue.empty()) {
		FileMessage msg = log_queue.front();
		log_queue.pop();

		// 调用原始的 LogFunc (现在它在主线程上是安全的)
		// 注意：LogFunc 内部需要被修改，不再需要锁和 HandleScope，因为它现在是被主线程调用的。
		// 为了避免修改 LogFunc，我们在这里直接调用其逻辑：
		//std::string logStr = WcharToUtf8(msg.message.c_str());

		// ... (V8 调用逻辑，使用 logCallback 和 isolate) ...
		if (msg.messageType == 0)
		{
			LogBase(msg.message);
		}
		else
		{
			reportBase(msg.type, msg.path);
		}
	}
}

static void OnExit(void* arg) {
	std::wcout << L"\nMonitoring stopped by process exit" << std::endl;
}

std::filesystem::path get_file_ext(std::wstring file_name) {
	std::filesystem::path file_path = file_name;
	std::filesystem::path ext = file_path.extension();
    return ext;
}

bool is_target_file_ext(std::wstring file_name) {
	std::filesystem::path ext = get_file_ext(file_name);

	// 2. 检查是否有扩展名
	if (ext.empty()) {
		return false;
	}
	else {
		return target_extensions.find(ext.wstring()) != target_extensions.end();
	}
}

bool is_file_locked_by_RM(const std::wstring& filePath) {
	DWORD sessionHandle;
	WCHAR sessionKey[CCH_RM_SESSION_KEY + 1] = { 0 };

	// 创建重启管理器会话
	DWORD result = RmStartSession(&sessionHandle, 0, sessionKey);
	if (result != ERROR_SUCCESS) {
		return false;
	}

	// 注册要检查的文件
	PCWSTR files[] = { filePath.c_str() };
	result = RmRegisterResources(sessionHandle, 1, files, 0, NULL, 0, NULL);
	if (result != ERROR_SUCCESS) {
		RmEndSession(sessionHandle);
		return false;
	}

	// 获取文件使用信息
	DWORD reason;
	UINT processInfoNeeded;
	UINT processInfoCount = 0;
	RM_PROCESS_INFO* processInfo = NULL;

	result = RmGetList(sessionHandle, &processInfoNeeded, &processInfoCount,
		processInfo, &reason);

	RmEndSession(sessionHandle);

	// 如果有进程在使用该文件
	return (result == ERROR_MORE_DATA && processInfoNeeded > 0);
}

// 简单的宽字符转UTF-8 string 辅助函数
std::string WStringToString(const std::wstring& wstr) {
	if (wstr.empty()) return std::string();
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
	std::string strTo(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
	return strTo;
}

class DirectoryMonitor {
private:
	std::wstring directory_path_;
	HANDLE directory_handle_;
	std::atomic<bool> stop_monitoring_;
	std::thread monitor_thread_;

	// --- 新增：用于去重的结构 ---
	struct FileEventState {
		DWORD last_action;
		std::chrono::steady_clock::time_point last_time;
	};

	// 记录文件路径对应的状态
	std::map<std::wstring, FileEventState> event_cache_;
	// 冷却时间（毫秒）：在此时间内重复的事件会被忽略
	const int kCooldownMs = 500;
	// --- 结束新增 ---

public:
	DirectoryMonitor(const std::wstring& directory_path)
		: directory_path_(directory_path), directory_handle_(INVALID_HANDLE_VALUE), stop_monitoring_(false) {
	}

	~DirectoryMonitor() {
		stop();
	}

	bool start() {
		// 打开目录句柄
		directory_handle_ = CreateFileW(
			directory_path_.c_str(),
			FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			NULL,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			NULL
		);

		if (directory_handle_ == INVALID_HANDLE_VALUE) {
			std::wcerr << L"cannot open directory: " << directory_path_
				<< L", error code: " << GetLastError() << std::endl;
			return false;
		}

		stop_monitoring_ = false;
		monitor_thread_ = std::thread(&DirectoryMonitor::monitor_loop, this);
		return true;
	}

	void join() {
		if (monitor_thread_.joinable())
		{
			monitor_thread_.join();
		}
	}

	void stop() {
		stop_monitoring_ = true;
		if (directory_handle_ != INVALID_HANDLE_VALUE) {
			CancelIo(directory_handle_);
			CloseHandle(directory_handle_);
			directory_handle_ = INVALID_HANDLE_VALUE;
		}
		if (monitor_thread_.joinable()) {
			monitor_thread_.join();
		}
	}

private:
	std::wstring find_original_file(const std::wstring& directory, const std::wstring& tempFileName) {
		// 1. 提取共同后缀 (去掉 ~$)
		if (tempFileName.length() < 3) return UNKUOWN;
		std::wstring suffix = tempFileName.substr(2); // 比如 ~$port.docx -> port.docx

		// 2. 遍历目录寻找匹配项
		std::wstring searchPattern = directory + L"\\*" + suffix; // 搜索 *port.docx

		WIN32_FIND_DATAW findData;
		HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);

		if (hFind == INVALID_HANDLE_VALUE) return UNKUOWN_NO_ORIGIN;

		std::wstring result = L"";
		do {
			std::wstring currentFile = findData.cFileName;
			// 排除临时文件自己
			if (currentFile == tempFileName) continue;

			// 简单的长度匹配检查 (Office 只是替换前两个字符，长度通常不变)
			if (currentFile == suffix) {
				result = currentFile;
				break; // 找到一个匹配的就停止
			}
			else if (currentFile.find(suffix) != std::wstring::npos && 
				currentFile.length() == (suffix.length() + 2))
			{
				result = currentFile;
				break; // 找到一个匹配的就停止
			}
		} while (FindNextFileW(hFind, &findData));

		FindClose(hFind);

		if (result.empty()) return UNKUOWN_MAYBE_DEL;
		return result;
	}

	// --- 新增：核心过滤逻辑 ---
	bool should_process_event(const std::wstring& full_path, DWORD action) {
		auto now = std::chrono::steady_clock::now();
		auto it = event_cache_.find(full_path);

		// 如果是新文件（之前没记录），或者记录已经超时（为了清理内存，这里简化处理，
		// 实际可以将超时的记录视为新记录）
		if (it == event_cache_.end()) {
			// 如果是新记录，总是允许，并保存状态
			event_cache_[full_path] = { action, now };
			clean_cache_if_needed(); // 简单的清理机制
			return true;
		}

		auto& state = it->second;
		long long diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_time).count();

		// 如果时间间隔超过冷却时间，视为新的一轮操作，允许处理
		if (diff_ms > kCooldownMs) {
			state.last_action = action;
			state.last_time = now;
			return true;
		}

		// --- 进入冷却时间内的逻辑判断 ---

		// 逻辑 1: 如果这次是 MODIFIED，但上次（在很短时间内）是 ADDED
		// 意味着是 "创建后立即写入"，我们要忽略这次 MODIFIED
		if (action == FILE_ACTION_MODIFIED && state.last_action == FILE_ACTION_ADDED) {
			// 只更新时间，保持状态为 ADDED，这样后续连续的 MODIFIED 也会被忽略
			state.last_time = now;
			return false;
		}

		// 逻辑 2: 如果这次是 MODIFIED，上次也是 MODIFIED
		// 意味着是 "连续写入"，我们要忽略后续的 MODIFIED
		if (action == FILE_ACTION_MODIFIED && state.last_action == FILE_ACTION_MODIFIED) {
			state.last_time = now;
			return false;
		}

		// 其他情况（例如 ADDED，虽然理论上同一文件不会连续 ADDED，但如果发生则更新）
		state.last_action = action;
		state.last_time = now;
		return true;
	}

	// 防止 map 无限增长的简单清理
	void clean_cache_if_needed() {
		if (event_cache_.size() > 1000) {
			auto now = std::chrono::steady_clock::now();
			for (auto it = event_cache_.begin(); it != event_cache_.end(); ) {
				long long diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.last_time).count();
				// 如果超过5秒没动静，就从缓存清除
				if (diff_ms > 5000) {
					it = event_cache_.erase(it);
				}
				else {
					++it;
				}
			}
		}
	}
	// --- 结束新增 ---

	void monitor_loop() {
		const DWORD buffer_size = 64 * 1024; // 64KB 缓冲区
		std::vector<BYTE> buffer(buffer_size);
		OVERLAPPED overlapped = { 0 };
		HANDLE events[2] = { 0 };

		// 创建事件用于异步操作
		events[0] = CreateEvent(NULL, TRUE, FALSE, NULL);
		events[1] = CreateEvent(NULL, TRUE, FALSE, NULL);
		overlapped.hEvent = events[0];

		if (!events[0] || !events[1]) {
			std::wcerr << L"cannot create events" << std::endl;
			return;
		}

		while (!stop_monitoring_) {
			DWORD bytes_returned = 0;

			// 开始异步监控
			BOOL success = ReadDirectoryChangesW(
				directory_handle_,
				buffer.data(),
				buffer_size,
				TRUE, // 监控子目录
				//FILE_NOTIFY_CHANGE_LAST_ACCESS |   // 文件访问（包括打开）
				FILE_NOTIFY_CHANGE_FILE_NAME |     // 文件创建/删除/重命名
				FILE_NOTIFY_CHANGE_LAST_WRITE   // 文件修改
				//FILE_NOTIFY_CHANGE_SIZE |          // 文件大小变化
				//FILE_NOTIFY_CHANGE_CREATION |       // 文件创建时间变化
				//FILE_NOTIFY_CHANGE_ATTRIBUTES | 
				//FILE_NOTIFY_CHANGE_SECURITY
				,
				&bytes_returned,
				&overlapped,
				NULL
			);

			if (!success) {
				DWORD error = GetLastError();
				if (error != ERROR_IO_PENDING) {
					std::wcerr << L"ReadDirectoryChangesW failed, error code: " << error << std::endl;
					break;
				}
			}

			// 等待目录变化或停止信号
			DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, INFINITE);

			if (wait_result == WAIT_OBJECT_0) {
				// 目录发生变化
				if (GetOverlappedResult(directory_handle_, &overlapped, &bytes_returned, FALSE)) {
					if (bytes_returned > 0) {
						process_changes(buffer.data(), bytes_returned);
					}
				}

				// 重置事件，准备下一次监控
				ResetEvent(events[0]);
				overlapped.Offset = 0;
				overlapped.OffsetHigh = 0;
			}
			else if (wait_result == WAIT_OBJECT_0 + 1) {
				// 收到停止信号
				break;
			}
		}

		CloseHandle(events[0]);
		CloseHandle(events[1]);
	}

	void process_changes(BYTE* buffer, DWORD buffer_size) {
		FILE_NOTIFY_INFORMATION* notify_info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);

		do {
			std::wstring file_name(notify_info->FileName, notify_info->FileNameLength / sizeof(WCHAR));
			std::wstring full_path = directory_path_ + L"\\" + file_name;
			std::wstring original_name = L"";

			// 1. 获取动作类型
			DWORD current_action = notify_info->Action;

			// 2. 检查是否应该处理此事件 (核心过滤逻辑)
			// 注意：我们只过滤 ADDED 和 MODIFIED，removed 通常不需要防抖
			bool process_it = true;
			if (current_action == FILE_ACTION_ADDED || current_action == FILE_ACTION_MODIFIED) {
				if (!should_process_event(full_path, current_action)) {
					process_it = false;
				}
			}

			if (process_it) 
			{
				switch (notify_info->Action) {
					case FILE_ACTION_ADDED:
					{
						original_name = find_original_file(directory_path_, file_name);
						bool is_target = is_target_file_ext(original_name);
						if (original_name != UNKUOWN && original_name != UNKUOWN_MAYBE_DEL && original_name != UNKUOWN_NO_ORIGIN && is_target)
						{
							std::wstring path = directory_path_ + L"\\" + original_name;
							std::cout << "[create] " << WStringToString(path) << std::endl;
							std::filesystem::path ext = get_file_ext(file_name);
							asyncReport(ext.wstring(), path);
						}
						break;
					}
					case FILE_ACTION_REMOVED:
					{
						// 删除事件通常不通过防抖逻辑，或者你可以选择清理 cache
						event_cache_.erase(full_path);
						//std::cout << "[delete] " << WStringToString(full_path) << std::endl;
						break;
					}
					case FILE_ACTION_MODIFIED:
					{
						// 因为 should_process_event 已经过滤了多余的 MODIFIED
						// 这里的代码只会在第一次 MODIFIED 或者 ADDED 很久之后的 MODIFIED 执行
						// 过滤掉目录更改，只看文件
						// 检查文件属性以确认是否是访问事件
						DWORD attributes = GetFileAttributesW(full_path.c_str());
						if (attributes != INVALID_FILE_ATTRIBUTES &&
							!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
							// 这里可以添加额外的逻辑来确认是文件打开
							// 例如检查文件是否被特定进程锁定等
							bool is_locked = is_file_locked_by_RM(full_path);
							bool is_target = is_target_file_ext(full_path);
							if (is_locked && is_target)
							{
								std::cout << "file modified: " << WStringToString(full_path) << std::endl;
								std::filesystem::path ext = get_file_ext(full_path);
								asyncReport(ext.wstring(), full_path);
							}
						}
					}
				}
			}

			// 移动到下一个通知
			if (notify_info->NextEntryOffset == 0) {
				break;
			}
			notify_info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
				reinterpret_cast<BYTE*>(notify_info) + notify_info->NextEntryOffset);
		} while (true);
	}
};

std::vector<std::shared_ptr<DirectoryMonitor>> monitors;
// 工具函数：获取更详细的文件访问信息
bool is_file_likely_opened(const std::wstring& file_path) {
	// 尝试以独占方式打开文件，如果失败则说明文件可能已被其他进程打开
	HANDLE hFile = CreateFileW(
		file_path.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ, // 允许其他进程读取
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	if (hFile == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		if (error == ERROR_SHARING_VIOLATION) {
			// 文件被其他进程以独占方式打开
			return true;
		}
		return false;
	}

	CloseHandle(hFile);
	return false;
}

static void WatchInitialize(const v8::FunctionCallbackInfo<v8::Value>& args) {
	isolate = args.GetIsolate();

	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	// 检查参数是否有效
	if (args.Length() < 4) {
		isolate->ThrowException(v8::Exception::TypeError(
			v8::String::NewFromUtf8(isolate, "必须传入两个回调函数和一个字符串数组").ToLocalChecked()));
		return;
	}
	if (!args[0]->IsArray()) {
		isolate->ThrowException(v8::Exception::TypeError(
			v8::String::NewFromUtf8(isolate, "第一个参数必须是字符串数组").ToLocalChecked()));
		return;
	}
	if (!args[1]->IsArray()) {
		isolate->ThrowException(v8::Exception::TypeError(
			v8::String::NewFromUtf8(isolate, "第二个参数必须是字符串数组").ToLocalChecked()));
		return;
	}
	if (!args[2]->IsFunction()) {
		isolate->ThrowException(v8::Exception::TypeError(
			v8::String::NewFromUtf8(isolate, "第三个参数必须是回调函数").ToLocalChecked()));
		return;
	}
	if (!args[3]->IsFunction()) {
		isolate->ThrowException(v8::Exception::TypeError(
			v8::String::NewFromUtf8(isolate, "第四个参数必须是回调函数").ToLocalChecked()));
		return;
	}

	uv_async_init(uv_default_loop(), &async_log_handle, AsyncLogCallback);

	node::Environment* env = node::GetCurrentEnvironment(isolate->GetCurrentContext());
	if (env)
	{
		node::AtExit(env, OnExit, nullptr);
	}
	else {
		std::cerr << "env is null" << std::endl;
		LogError(L"env is null");
	}


	fileCallback = v8::Local<v8::Function>::Cast(args[2]);
	logCallback = v8::Local<v8::Function>::Cast(args[3]);

	v8::Local<v8::Array> jsArray = v8::Local<v8::Array>::Cast(args[0]);
	uint32_t arrayLength = jsArray->Length();
	std::set<std::wstring> targetExtensions;

	for (uint32_t i = 0; i < arrayLength; i++) {
		v8::Local<v8::Value> element;
		if (jsArray->Get(context, i).ToLocal(&element)) {
			if (element->IsString()) {
				// 将JavaScript字符串转换为std::string
				v8::String::Utf8Value utf8Str(isolate, element);
				if (*utf8Str) {
					// 转换为std::wstring并添加到集合
					std::wstring wstr = Utf8ToWstring(*utf8Str);
					targetExtensions.insert(wstr);
				}
			}
		}
	}
	target_extensions = targetExtensions;
	std::wstring setContents;
	for (std::wstring wstr : targetExtensions) {
		setContents += wstr + L" ";
	}
	LogInfo(L"Target extensions: " + setContents);

	std::vector<std::wstring> targetDirectory;
	jsArray = v8::Local<v8::Array>::Cast(args[1]);
	arrayLength = jsArray->Length();
	for (uint32_t i = 0; i < arrayLength; i++) {
		v8::Local<v8::Value> element;
		if (jsArray->Get(context, i).ToLocal(&element)) {
			if (element->IsString()) {
				// 将JavaScript字符串转换为std::string
				v8::String::Utf8Value utf8Str(isolate, element);
				if (*utf8Str) {
					// 转换为std::wstring并添加到集合
					std::wstring wspath = Utf8ToWstring(*utf8Str);
					targetDirectory.push_back(wspath);
				}
			}
		}
	}
	std::wstring vectorContents;
	for (std::wstring wspath : targetDirectory) {
		vectorContents += wspath + L" ";
		// 监听目录
		std::shared_ptr<DirectoryMonitor> monitor_ptr = std::make_shared<DirectoryMonitor>(wspath);
		if (monitor_ptr->start()) {
			LogInfo(L"beginning to monitor directory: " + wspath);
			monitors.push_back(monitor_ptr);
		}
		else 
		{
			LogError(wspath + L", start monitor failed");
		}
	}
	LogInfo(L"Target directories: " + vectorContents);

	for (std::shared_ptr<DirectoryMonitor> monitor : monitors)
	{
		monitor->join();
	}
}

static void HandleExist(uv_signal_s* handle, int signal) {
	LogInfo(L"file watch stopped by user");
}

void Initialize(v8::Local<v8::Object> exports) {
	NODE_SET_METHOD(exports, "WatchInitialize", WatchInitialize);

	uv_signal_t* signalHandler = new uv_signal_t;
	uv_signal_init(uv_default_loop(), signalHandler);
	uv_signal_start(signalHandler, HandleExist, SIGTERM);
}

NODE_MODULE(NODE_GYP_MODULE_NAME, Initialize)

// 使用示例
/*
int main() {
	SetConsoleOutputCP(CP_UTF8);

	PWSTR path = nullptr;

	// 获取桌面路径
	// 使用 FOLDERID_Desktop
	std::wstring desktop_path;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &path))) {
		desktop_path = std::wstring(path);
		CoTaskMemFree(path); // 必须释放内存
	}
	std::wcout << L"Desktop path: " << desktop_path << std::endl;
	// 获取下载文件夹路径
	// 使用 FOLDERID_Downloads
	std::wstring download_path;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &path))) {
		download_path = std::wstring(path);
		CoTaskMemFree(path);
	}
	std::wcout << L"Download path: " << download_path << std::endl;

	DirectoryMonitor download_monitor(download_path);

	if (download_monitor.start()) {
		std::cout << "beginning to monitor directory: " << WStringToString(download_path) << std::endl;
	}
	else {
		std::wcerr << L"can not start download monitor" << std::endl;
		return 1;
	}
	
	DirectoryMonitor desktop_monitor(desktop_path);

	if (desktop_monitor.start())
	{
		std::cout << "beginning to monitor directory: " << WStringToString(desktop_path) << std::endl;
	} 
	else
	{
		std::wcerr << L"can not start desktop monitor" << std::endl;
		download_monitor.stop();
		return 1;
	}

	std::cin.get(); // 等待用户输入

	download_monitor.stop();
	desktop_monitor.stop();

	std::cout << "all monitor already stopped" << std::endl;

	return 0;
}
*/