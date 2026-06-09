// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Input/Win32RawInputSource.h"

#ifdef _WIN32

#include "common/Console.h"

#include <fmt/format.h>

const wchar_t* Win32RawInputSource::WINDOW_CLASS_NAME = L"PCSX2_RawInput_Window_Class";
bool Win32RawInputSource::s_class_registered = false;
HINSTANCE Win32RawInputSource::s_hinstance = nullptr;
Win32RawInputSource* Win32RawInputSource::s_instance = nullptr;

Win32RawInputSource::Win32RawInputSource() = default;

Win32RawInputSource::~Win32RawInputSource()
{
	if (m_initialized)
		Shutdown();
}

bool Win32RawInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	if (m_initialized)
		return true;

	s_hinstance = GetModuleHandleW(nullptr);
	if (!s_hinstance)
	{
		Console.Error("Win32RawInputSource: GetModuleHandleW failed");
		return false;
	}

	if (!RegisterDummyWindowClass(s_hinstance))
	{
		Console.Error("Win32RawInputSource: Failed to register dummy window class");
		return false;
	}

	if (!CreateDummyWindow())
	{
		Console.Error("Win32RawInputSource: Failed to create dummy window");
		return false;
	}

	s_instance = this;

	if (!ReloadDevices())
	{
		Console.Warning("Win32RawInputSource: No raw input devices found on startup");
	}

	m_initialized = true;
	Console.WriteLn("Win32RawInputSource: Initialized successfully");
	return true;
}

void Win32RawInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	// No runtime settings for now — device enumeration handles hotplug
}

void Win32RawInputSource::Shutdown()
{
	if (!m_initialized)
		return;

	UnregisterRawInput();
	DestroyDummyWindow();

	s_instance = nullptr;
	m_mice.clear();
	m_initialized = false;

	Console.WriteLn("Win32RawInputSource: Shutdown complete");
}

bool Win32RawInputSource::IsInitialized()
{
	return m_initialized;
}

bool Win32RawInputSource::RegisterDummyWindowClass(HINSTANCE hinstance)
{
	if (s_class_registered)
		return true;

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.lpfnWndProc = DummyWindowProc;
	wc.hInstance = hinstance;
	wc.lpszClassName = WINDOW_CLASS_NAME;

	if (!RegisterClassExW(&wc))
	{
		Console.Error("Win32RawInputSource: RegisterClassExW failed: %u", GetLastError());
		return false;
	}

	s_class_registered = true;
	return true;
}

LRESULT CALLBACK Win32RawInputSource::DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_INPUT && s_instance)
	{
		UINT dwSize = 0;
		GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));

		if (dwSize > 0)
		{
			std::vector<BYTE> buffer(dwSize);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer.data(), &dwSize, sizeof(RAWINPUTHEADER)) == dwSize)
			{
				const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
				s_instance->ProcessRawInputEvent(*raw);
			}
		}
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool Win32RawInputSource::CreateDummyWindow()
{
	m_dummy_window = CreateWindowExW(0, WINDOW_CLASS_NAME, L"PCSX2 RawInput",
		0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, s_hinstance, nullptr);

	if (!m_dummy_window)
	{
		Console.Error("Win32RawInputSource: CreateWindowExW failed: %u", GetLastError());
		return false;
	}

	return true;
}

void Win32RawInputSource::DestroyDummyWindow()
{
	if (m_dummy_window)
	{
		DestroyWindow(m_dummy_window);
		m_dummy_window = nullptr;
	}
}

void Win32RawInputSource::EnsureRawInputRegistered()
{
	if (m_raw_input_registered)
		return;

	RAWINPUTDEVICE rid;
	rid.usUsagePage = 0x01; // Generic Desktop Controls
	rid.usUsage = 0x02;     // Mouse
	rid.dwFlags = RIDEV_INPUTSINK; // Receive events even when not in foreground
	rid.hwndTarget = m_dummy_window;

	if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE)))
	{
		Console.Error("Win32RawInputSource: RegisterRawInputDevices failed: %u", GetLastError());
		return;
	}

	m_raw_input_registered = true;
	Console.WriteLn("Win32RawInputSource: Raw input registered on hidden window");
}

void Win32RawInputSource::UnregisterRawInput()
{
	if (!m_raw_input_registered)
		return;

	RAWINPUTDEVICE rid;
	rid.usUsagePage = 0x01; // Generic Desktop Controls
	rid.usUsage = 0x02;     // Mouse
	rid.dwFlags = RIDEV_REMOVE;
	rid.hwndTarget = nullptr;

	RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));
	m_raw_input_registered = false;
}

bool Win32RawInputSource::IsAcceptableRawInputMouse(HANDLE hDevice)
{
	// Query device info to determine if this is a real pointing device.
	UINT device_info_size = sizeof(RID_DEVICE_INFO);
	RID_DEVICE_INFO device_info = {};

	if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &device_info, &device_info_size) != device_info_size)
		return false;

	if (device_info.dwType != RIM_TYPEMOUSE)
		return false;

	const DWORD id = device_info.mouse.dwId;
	const DWORD num_buttons = device_info.mouse.dwNumberOfButtons;

	// Filter out devices that present as a mouse but have zero buttons.
	// Some keyboards (e.g. Corsair) and HID-compliant devices register as
	// mouse-class devices without actual pointing capability.
	if (num_buttons == 0)
	{
		Console.WriteLn("Win32RawInputSource: Skipping device ID %u — 0 buttons (likely not a real mouse)", id);
		return false;
	}

	// Skip virtual/software mice (e.g. remote desktop, VNC).
	// These often have dwId = 0 which indicates a virtual device.
	// We keep them if they have buttons though (could be a real touchpad).
	if (id == 0)
	{
		Console.WriteLn("Win32RawInputSource: Device has dwId=0 (possibly virtual), keeping with %u buttons", num_buttons);
	}

	return true;
}

std::string Win32RawInputSource::GetDeviceName(HANDLE hDevice)
{
	// Get the device path string
	UINT name_size = 0;
	if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, nullptr, &name_size) != 0)
		return fmt::format("Mouse {}", reinterpret_cast<size_t>(hDevice));

	std::vector<WCHAR> name_buffer(name_size);
	if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, name_buffer.data(), &name_size) == 0)
		return fmt::format("Mouse {}", reinterpret_cast<size_t>(hDevice));

	// Convert wide string to UTF-8
	int utf8_len = WideCharToMultiByte(CP_UTF8, 0, name_buffer.data(), -1, nullptr, 0, nullptr, nullptr);
	if (utf8_len <= 0)
		return fmt::format("Mouse {}", reinterpret_cast<size_t>(hDevice));

	std::vector<char> utf8_buffer(utf8_len);
	WideCharToMultiByte(CP_UTF8, 0, name_buffer.data(), -1, utf8_buffer.data(), utf8_len, nullptr, nullptr);
	return std::string(utf8_buffer.data());
}

bool Win32RawInputSource::ReloadDevices()
{
	// Enumerate all raw input devices
	UINT num_devices = 0;
	if (GetRawInputDeviceList(nullptr, &num_devices, sizeof(RAWINPUTDEVICELIST)) != 0)
	{
		Console.Error("Win32RawInputSource: GetRawInputDeviceList size query failed");
		return false;
	}

	if (num_devices == 0)
		return false;

	std::vector<RAWINPUTDEVICELIST> device_list(num_devices);
	if (GetRawInputDeviceList(device_list.data(), &num_devices, sizeof(RAWINPUTDEVICELIST)) == UINT_MAX)
	{
		Console.Error("Win32RawInputSource: GetRawInputDeviceList failed");
		return false;
	}

	// Track existing device handles for disconnect detection
	std::vector<HANDLE> old_handles;
	for (const MouseState& ms : m_mice)
		old_handles.push_back(ms.device);

	// Filter to acceptable mice
	std::vector<RAWINPUTDEVICELIST> mice;
	for (UINT i = 0; i < num_devices; i++)
	{
		if (device_list[i].dwType == RIM_TYPEMOUSE)
		{
			if (IsAcceptableRawInputMouse(device_list[i].hDevice))
				mice.push_back(device_list[i]);
		}
	}

	// Limit to MAX_POINTER_DEVICES
	if (mice.size() > InputManager::MAX_POINTER_DEVICES)
	{
		Console.Warning("Win32RawInputSource: Found %u mice, limiting to %u", static_cast<unsigned>(mice.size()), InputManager::MAX_POINTER_DEVICES);
		mice.resize(InputManager::MAX_POINTER_DEVICES);
	}

	// Check for disconnected devices
	for (size_t i = 0; i < m_mice.size(); i++)
	{
		bool found = false;
		for (const RAWINPUTDEVICELIST& rid : mice)
		{
			if (rid.hDevice == m_mice[i].device)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			Console.WriteLn("Win32RawInputSource: Device disconnected: Pointer-%u", m_mice[i].pointer_index);
			InputManager::OnInputDeviceDisconnected(
				InputManager::MakePointerButtonKey(m_mice[i].pointer_index, 0),
				fmt::format("Pointer-{}", m_mice[i].pointer_index));
		}
	}

	// Update device list
	m_mice.clear();
	for (u32 i = 0; i < static_cast<u32>(mice.size()); i++)
	{
		MouseState ms = {};
		ms.device = mice[i].hDevice;
		ms.button_state = 0;
		ms.last_x = 0;
		ms.last_y = 0;
		ms.pointer_index = i;

		const std::string device_name = GetDeviceName(ms.device);
		const std::string identifier = fmt::format("Pointer-{}", i);

		m_mice.push_back(ms);

		Console.WriteLn("Win32RawInputSource: Device connected: %s (%s)", identifier.c_str(), device_name.c_str());
		InputManager::OnInputDeviceConnected(identifier, device_name);
	}

	// Ensure raw input is registered after device enumeration
	EnsureRawInputRegistered();

	Console.WriteLn("Win32RawInputSource: %u devices enumerated", static_cast<unsigned>(m_mice.size()));
	return !m_mice.empty();
}

s32 Win32RawInputSource::FindDeviceIndex(HANDLE hDevice) const
{
	for (size_t i = 0; i < m_mice.size(); i++)
	{
		if (m_mice[i].device == hDevice)
			return static_cast<s32>(i);
	}
	return -1;
}

bool Win32RawInputSource::ProcessRawInputEvent(const RAWINPUT& ev)
{
	if (ev.header.dwType != RIM_TYPEMOUSE)
		return false;

	const s32 device_index = FindDeviceIndex(ev.header.hDevice);
	if (device_index < 0)
		return false;

	const RAWMOUSE& mouse = ev.data.mouse;
	MouseState& state = m_mice[device_index];
	const u32 pointer_index = state.pointer_index;

	// Handle absolute movement (lightguns, touchscreens, digitizers)
	if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
	{
		const int screen_width = GetSystemMetrics(SM_CXSCREEN);
		const int screen_height = GetSystemMetrics(SM_CYSCREEN);

		// Absolute coordinates are in the range [0, 65535], map to screen pixels.
		// For virtual desktop spanning multiple monitors, use SM_CXVIRTUALSCREEN.
		const int virtual_screen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		const int virtual_screen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		const int virtual_screen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
		const int virtual_screen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);

		const float normalized_x =
			(static_cast<float>(mouse.lLastX) / static_cast<float>(USHRT_MAX)) * static_cast<float>(virtual_screen_w);
		const float normalized_y =
			(static_cast<float>(mouse.lLastY) / static_cast<float>(USHRT_MAX)) * static_cast<float>(virtual_screen_h);

		// Store for tracking
		state.last_x = mouse.lLastX;
		state.last_y = mouse.lLastY;

		InputManager::UpdatePointerAbsolutePosition(pointer_index, normalized_x, normalized_y);

		DevCon.WriteLn("Win32RawInputSource: Absolute move Pointer-%u → (%.1f, %.1f)",
			pointer_index, normalized_x, normalized_y);
	}
	else
	{
		// Handle relative movement (standard mice)
		if (mouse.lLastX != 0)
		{
			InputManager::UpdatePointerRelativeDelta(pointer_index, InputPointerAxis::X,
				static_cast<float>(mouse.lLastX), true);
		}
		if (mouse.lLastY != 0)
		{
			InputManager::UpdatePointerRelativeDelta(pointer_index, InputPointerAxis::Y,
				static_cast<float>(mouse.lLastY), true);
		}
	}

	// Handle button state changes via XOR with previous state
	const u32 prev_button_state = state.button_state;

	// Raw mouse button flags are in usButtonFlags
	// RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_1_UP, etc.
	static constexpr u16 button_masks[] = {
		RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_1_UP,
		RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_2_UP,
		RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_3_UP,
		RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP,
		RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP,
	};

	for (u32 btn = 0; btn < std::size(button_masks) / 2; btn++)
	{
		const u16 down_flag = button_masks[btn * 2];
		const u16 up_flag = button_masks[btn * 2 + 1];
		const u32 btn_mask = (1u << btn);

		if (mouse.usButtonFlags & down_flag)
		{
			state.button_state |= btn_mask;
		}
		else if (mouse.usButtonFlags & up_flag)
		{
			state.button_state &= ~btn_mask;
		}
	}

	// Fire events for changed buttons
	if (state.button_state != prev_button_state)
	{
		for (u32 btn = 0; btn < std::size(button_masks) / 2; btn++)
		{
			const u32 btn_mask = (1u << btn);
			const u32 was_pressed = (prev_button_state & btn_mask);
			const u32 is_pressed = (state.button_state & btn_mask);

			if (was_pressed != is_pressed)
			{
				const InputBindingKey key = InputManager::MakePointerButtonKey(pointer_index, btn);
				InputManager::InvokeEvents(key, is_pressed ? 1.0f : 0.0f);
			}
		}
	}

	return true;
}

void Win32RawInputSource::PollEvents()
{
	if (!m_dummy_window)
		return;

	// Pump messages for the dummy window to process WM_INPUT
	MSG msg;
	while (PeekMessageW(&msg, m_dummy_window, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

std::vector<std::pair<std::string, std::string>> Win32RawInputSource::EnumerateDevices()
{
	std::vector<std::pair<std::string, std::string>> ret;

	for (const MouseState& ms : m_mice)
	{
		const std::string identifier = fmt::format("Pointer-{}", ms.pointer_index);
		const std::string name = GetDeviceName(ms.device);
		ret.emplace_back(identifier, name);
	}

	return ret;
}

std::vector<InputBindingKey> Win32RawInputSource::EnumerateMotors()
{
	// Mice and lightguns don't have vibration motors
	return {};
}

bool Win32RawInputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	// Pointer devices don't have generic controller bindings
	return false;
}

InputLayout Win32RawInputSource::GetControllerLayout(u32 index)
{
	// Pointer devices are not controllers
	return InputLayout::Unknown;
}

void Win32RawInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
	// No motors to update
}

std::optional<InputBindingKey> Win32RawInputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
	// Pointer bindings are handled by InputManager's built-in ParsePointerKey.
	// We don't need to handle them here.
	return std::nullopt;
}

TinyString Win32RawInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	TinyString ret;

	if (key.source_subtype == InputSubclass::PointerButton)
	{
		ret.format("Pointer-{} Button{}", u32{key.source_index}, key.data + 1);
	}
	else if (key.source_subtype == InputSubclass::PointerAxis)
	{
		static constexpr const char* axis_names[] = {"X", "Y", "Wheel X", "Wheel Y"};
		const u32 axis = key.data;
		if (axis < std::size(axis_names))
			ret.format("Pointer-{} {}", u32{key.source_index}, axis_names[axis]);
		else
			ret.format("Pointer-{} Axis{}", u32{key.source_index}, axis);
	}
	else
	{
		ret.format("Pointer-{} Unknown", u32{key.source_index});
	}

	return ret;
}

TinyString Win32RawInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	// No icons for pointer devices
	return {};
}

#endif // _WIN32
