// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef _WIN32

#include "Input/InputSource.h"

#include <functional>
#include <mutex>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class SettingsInterface;

/// RawInput input source using a hidden window to receive WM_INPUT messages.
/// Enables multiple independent pointer devices (mice, lightguns) by reading
/// Raw Input data directly instead of going through the system cursor.
/// Each physical mouse/lightgun is mapped to a Pointer-X device index.
class Win32RawInputSource final : public InputSource
{
public:
	Win32RawInputSource();
	~Win32RawInputSource() override;

	bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	bool ReloadDevices() override;
	void Shutdown() override;
	bool IsInitialized() override;

	void PollEvents() override;

	std::optional<InputBindingKey> ParseKeyString(const std::string_view device, const std::string_view binding) override;
	TinyString ConvertKeyToString(InputBindingKey key, bool display = false, bool migration = false) override;
	TinyString ConvertKeyToIcon(InputBindingKey key) override;

	std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
	std::vector<InputBindingKey> EnumerateMotors() override;
	bool GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping) override;
	InputLayout GetControllerLayout(u32 index) override;
	void UpdateMotorState(InputBindingKey key, float intensity) override;

private:
	/// Per-device state tracking for raw input mice.
	struct MouseState
	{
		HANDLE device;       ///< Raw Input device handle (from GetRawInputDeviceInfo)
		u32 button_state;    ///< Current button state bitmask
		s32 last_x;          ///< Last absolute X position
		s32 last_y;          ///< Last absolute Y position
		u32 pointer_index;   ///< Assigned Pointer-X index
	};

	// Hidden window for receiving WM_INPUT
	static const wchar_t* WINDOW_CLASS_NAME;

	static bool s_class_registered;
	static HINSTANCE s_hinstance;
	static Win32RawInputSource* s_instance;

	static bool RegisterDummyWindowClass(HINSTANCE hinstance);
	static LRESULT CALLBACK DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	bool CreateDummyWindow();
	void DestroyDummyWindow();

	// Raw Input device management
	void EnsureRawInputRegistered();
	void UnregisterRawInput();

	/// Returns true if the device handle looks like a real mouse (not a keyboard with mouse buttons, etc.)
	static bool IsAcceptableRawInputMouse(HANDLE hDevice);

	/// Process a single WM_INPUT raw input event.
	bool ProcessRawInputEvent(const RAWINPUT& ev);

	/// Find the device index in m_mice by handle.
	s32 FindDeviceIndex(HANDLE hDevice) const;

	/// Get a human-readable name for a raw input device.
	static std::string GetDeviceName(HANDLE hDevice);

	HWND m_dummy_window = nullptr;
	std::vector<MouseState> m_mice;
	bool m_raw_input_registered = false;
	bool m_initialized = false;
};

#endif // _WIN32
