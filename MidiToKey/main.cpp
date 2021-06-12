#include <windows.h>
#include <winuser.h>
#include <stdio.h>
#include <conio.h>
#include <mmsystem.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <map>
#include <vector>
#include <fstream>
#include <string>

#pragma comment(lib, "winmm.lib")

// system exclusive long message buffer size
const size_t sysExBufSize = 80;

// MIDI In device name
std::wstring midiInDeviceName;

// flag for mapping process
std::atomic<bool> mapInProcess = false;

// flag to determine if MIDI note should be pressed first
std::atomic<bool> midiNotePressed = false;

// input buffer for System Exclusive messages
unsigned char SysXBuffer[256];

// flag to indicate whether I'm currently receiving a SysX message
unsigned char SysXFlag = 0;

// key holder thread
std::thread keyHolderThread;

// flag for holding key
std::atomic<bool> holdKey = false;

// Esc key code
const int escKeyCode = 27;

// sleep time before next SendInput
const std::chrono::milliseconds keyHoldDelay(25);

// note to sequence of keys map
std::map<unsigned int, std::vector<unsigned short>> noteToKeysMap;

// current note, selected on MIDI In device
std::atomic<unsigned int> currentNote;

// mapping file name
const std::string mappingFileName = "mapping.txt";

// mapping save result
enum class SaveResult { SaveNotCalled, Ok, Nok };
SaveResult mappingSaveResult = SaveResult::SaveNotCalled;

//////  FUNCTIONS ////////////////

void parseMappingLine(std::string line)
{
	std::stringstream stream { line };
	const std::vector<std::string> tokenized { std::istream_iterator<std::string>{stream}, std::istream_iterator<std::string>{} };

	unsigned int note = stoi(tokenized[0]);

	for (size_t i = 1; i < tokenized.size(); i++)
	{
		noteToKeysMap[note].push_back(stoi(tokenized[i]));
	}
}

bool saveMappingToFile()
{
	std::ofstream file(mappingFileName);

	if (file.is_open())
	{
		for (auto note : noteToKeysMap)
		{
			file << note.first;

			for (auto key : note.second)
			{
				file << " " << key;
			}

			file << "\n";
		}

		file.close();

		return true;
	}
	else
	{
		std::cout << "Unable to open and save to mapping file: " << mappingFileName << std::endl;
	}

	return false;
}

bool loadMappingFromFile()
{
	std::ifstream file(mappingFileName);

	if (file.is_open())
	{
		noteToKeysMap.clear();

		std::string line;

		while (std::getline(file, line))
		{
			parseMappingLine(line);
		}

		file.close();

		return true;
	}

	return false;
}

void printDeviceNameHeader()
{
	system("cls");
	std::wcout << L"MIDI In device selected: " << midiInDeviceName << std::endl;
	std::cout << "=============================================================" << std::endl << std::endl;
}

void printMapProcessInfo()
{
	printDeviceNameHeader();
	std::cout << "Press MIDI key, then press one or more keyboard keys to map" << std::endl;
	std::cout << "When finished with current MIDI key, just press next MIDI key, no need to confirm current mapping in any way" << std::endl;
	std::cout << "Press Esc anytime to stop mapping process and start normal usage" << std::endl << std::endl;
	if (!midiNotePressed)
	{
		std::cout << "Press MIDI key";
	}
	else
	{
		std::cout << "Press keyboard key(s)" << std::endl << std::endl;
		std::cout << "MIDI key " << currentNote << " mapped to -> ";

		for (size_t i = 0; i < noteToKeysMap[currentNote].size(); i++)
		{
			std::cout << noteToKeysMap[currentNote][i];

			if (i < noteToKeysMap[currentNote].size() - 1)
			{
				std::cout << " + ";
			}
		}
	}
}

void printNormalUsageProcessInfo()
{
	printDeviceNameHeader();

	if (mappingSaveResult != SaveResult::SaveNotCalled)
	{
		std::cout << "Mappings " << (mappingSaveResult == SaveResult::Ok ? "saved" : "not saved") << std::endl << std::endl;
	}

	std::cout << "Normal usage process (press Esc to exit the program)" << std::endl << std::endl;
}

// key "presser" thread, it "holds" down a key by calling SendInput multiple times until stopped
void keyHolderThreadFunc(std::vector<unsigned short> keys)
{
	INPUT ip;

	// Set up a generic keyboard event.
	ip.type = INPUT_KEYBOARD;
	ip.ki.wScan = 0; // hardware scan code for key
	ip.ki.time = 0;
	ip.ki.dwExtraInfo = 0;
	ip.ki.dwFlags = 0; // 0 for key press

	while (holdKey)
	{
		// Press the keys
		for (auto key : keys)
		{
			ip.ki.wVk = key;
			SendInput(1, &ip, sizeof(INPUT));
		}
		std::this_thread::sleep_for(keyHoldDelay);
	}

	// Release the keys
	ip.ki.dwFlags = KEYEVENTF_KEYUP;
	for (auto key : keys)
	{
		ip.ki.wVk = key;
		SendInput(1, &ip, sizeof(INPUT));
	}
}

// Callback routine for MIDI input
void CALLBACK midiCallback(HMIDIIN handle, UINT uMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2)
{
	LPMIDIHDR lpMIDIHeader;
	unsigned char* ptr;
	char buffer[sysExBufSize];
	unsigned char bytes;
	
	switch (uMsg)
	{
	// Received some regular MIDI message
	case MIM_DATA:
	{
		unsigned int type		= (dwParam1)		& 0x000000F0;
		unsigned int note		= (dwParam1 >> 8)	& 0x000000FF;
		unsigned int velocity	= (dwParam1 >> 16)	& 0x000000FF;

		if (type == 0x90 && velocity != 0) // key pressed
		{
			if (mapInProcess)
			{
				midiNotePressed = true;
				currentNote = note;
				noteToKeysMap[currentNote].clear();

				printMapProcessInfo();
			}
			else
			{
				holdKey = true;
				if (!keyHolderThread.joinable())
				{
					keyHolderThread = std::thread(keyHolderThreadFunc, noteToKeysMap[note]);
				}
			}
		}
		else // key released
		{
			if (!mapInProcess)
			{
				holdKey = false;
				if (keyHolderThread.joinable())
				{
					keyHolderThread.join();
				}
			}
		}

		break;
	}
	// Received all or part of some System Exclusive message
	case MIM_LONGDATA:
	{
		// If this application is ready to close down, then don't call midiInAddBuffer() again
		if (!(SysXFlag & 0x80))
		{
			// Assign address of MIDIHDR to a LPMIDIHDR variable. Makes it easier to access the field that contains the pointer to our block of MIDI events
			lpMIDIHeader = (LPMIDIHDR)dwParam1;
			
			// Get address of the MIDI event that caused this call
			ptr = (unsigned char*)(lpMIDIHeader->lpData);
			
			// Is this the first block of System Exclusive bytes?
			if (!SysXFlag)
			{
				/* Print out a noticeable heading as well as the timestamp of the first block.
				(But note that other, subsequent blocks will have their own time stamps). */
				std::cout << "*************** System Exclusive **************" << std::endl;
				std::cout << dwParam2 << std::endl;
				
				// Indicate we've begun handling a particular System Exclusive message
				SysXFlag |= 0x01;
			}
			
			// Is this the last block (i.e. the end of System Exclusive byte is here in the buffer)?
			if (*(ptr + (lpMIDIHeader->dwBytesRecorded - 1)) == 0xF7)
			{
				// Indicate we're done handling this particular System Exclusive message
				SysXFlag &= (~0x01);
			}
			
			// Display the bytes (16 per line)
			bytes = 16;
			while ((lpMIDIHeader->dwBytesRecorded--))
			{
				if (!(--bytes))
				{
					sprintf_s(&buffer[0], sysExBufSize, "0x%02X\r\n", *(ptr)++);
					bytes = 16;
				}
				else
				{
					sprintf_s(&buffer[0], sysExBufSize, "0x%02X ", *(ptr)++);
				}
				
				printf("%s", buffer);
				fflush(stdout);
			}
			
			// Was this the last block of System Exclusive bytes?
			if (!SysXFlag)
			{
				/* Print out a noticeable ending */
				std::cout << std::endl << "******************************************" << std::endl;
			}
			// Queue the MIDIHDR for more input
			midiInAddBuffer(handle, lpMIDIHeader, sizeof(MIDIHDR));
		}
		break;
	}
	// Process these messages if you desire
	case MIM_OPEN:
	{
		std::cout << "Midi In device opened" << std::endl;
		break;
	}
	case MIM_CLOSE:
	{
		std::cout << "Midi In device closed" << std::endl;
		break;
	}
	case MIM_ERROR:
	{
		std::cout << "MIM_ERROR" << std::endl;
		break;
	}
	case MIM_LONGERROR:
	{
		std::cout << "MIM_LONGERROR" << std::endl;
		break;
	}
	case MIM_MOREDATA:
	{
		std::cout << "MIM_MOREDATA" << std::endl;
		break;
	}
	default:
	{
		std::cout << "MIDI In message unknown: " << uMsg << std::endl;
	}
	}
}

size_t indexSelector(std::string msg, size_t maxCnt)
{
	size_t index = 0;

	do
	{
		std::cout << msg;
		std::cin >> index;

		if (index >= maxCnt)
		{
			std::cout << "Wrong index selected" << std::endl;
			continue;
		}
	} while (index >= maxCnt);

	return index;
}

size_t listAndSelectMidiInDevices()
{
	size_t midiInDeviceNum = midiInGetNumDevs();
	
	MIDIINCAPS midiInDeviceCapabilities;

	std::cout << "List of MIDI input devices:" << std::endl;
	std::cout << "---------------------------" << std::endl;

	for (size_t i = 0; i < midiInDeviceNum; i++)
	{
		midiInGetDevCaps(i, &midiInDeviceCapabilities, sizeof(MIDIINCAPS));
		std::wcout << i << L": " << midiInDeviceCapabilities.szPname << std::endl;
	}

	std::cout << "---------------------------" << std::endl << std::endl;

	size_t index = indexSelector("Select the device index: ", midiInDeviceNum);

	midiInGetDevCaps(index, &midiInDeviceCapabilities, sizeof(MIDIINCAPS));
	midiInDeviceName = midiInDeviceCapabilities.szPname;

	return index;
}

size_t listAndSelectUserOptions()
{
	printDeviceNameHeader();

	std::cout << "Mapping file " << (loadMappingFromFile() ? "found and loaded" : "not found / not loaded") << std::endl << std::endl;

	std::cout << "Select option:" << std::endl;
	std::cout << "---------------------------" << std::endl;
	std::cout << "0. Map keys" << std::endl;
	std::cout << "1. Start usage" << std::endl;
	std::cout << "---------------------------" << std::endl << std::endl;

	return indexSelector("", 2);
}

void mapNotesToKeyboardKeys()
{
	noteToKeysMap.clear();

	HANDLE hConIn = GetStdHandle(STD_INPUT_HANDLE);
	INPUT_RECORD in;
	DWORD result;

	printMapProcessInfo();

	do
	{
		if (!ReadConsoleInput(hConIn, &in, 1, &result) || !result)
		{
			std::cout << "Console input read error" << std::endl;
			return;
		}

		if (in.EventType == KEY_EVENT &&
			in.Event.KeyEvent.bKeyDown &&
			in.Event.KeyEvent.wVirtualKeyCode != VK_ESCAPE)
		{
			if (!midiNotePressed)
			{
				continue;
			}

			auto it = std::find(noteToKeysMap[currentNote].begin(), noteToKeysMap[currentNote].end(), in.Event.KeyEvent.wVirtualKeyCode);
			bool keyAlreadyMapped = (it != noteToKeysMap[currentNote].end());

			if (!keyAlreadyMapped)
			{
				noteToKeysMap[currentNote].push_back(in.Event.KeyEvent.wVirtualKeyCode);
				printMapProcessInfo();
			}
		}
	} while (in.Event.KeyEvent.wVirtualKeyCode != VK_ESCAPE);

	mapInProcess = false;
	mappingSaveResult = (saveMappingToFile() ? SaveResult::Ok : SaveResult::Nok);
}

void processKeypressing()
{
	int keyCode = -1;
	do
	{
		keyCode = _getch();
	} while (keyCode != escKeyCode);
}

void printMidiInError(std::string msg, unsigned long err)
{
	std::cout << "MIDI In error occured during " << msg << ": " << err << std::endl;
}

int main()
{
	size_t deviceIndex = listAndSelectMidiInDevices();
	size_t optionIndex = listAndSelectUserOptions();

	if (optionIndex == 0)
	{
		mapInProcess = true;
	}

	HMIDIIN handle;
	MIDIHDR midiHdr;
	unsigned long err;

	// Open selected MIDI In device
	if (!(err = midiInOpen(&handle, deviceIndex, (DWORD)midiCallback, 0, CALLBACK_FUNCTION)))
	{
		// Store pointer to our input buffer for System Exclusive messages in MIDIHDR
		midiHdr.lpData = (char*)(LPBYTE)&SysXBuffer[0];
		// Store its size in the MIDIHDR
		midiHdr.dwBufferLength = sizeof(SysXBuffer);
		// Flags must be set to 0
		midiHdr.dwFlags = 0;
		// Prepare the buffer and MIDIHDR
		err = midiInPrepareHeader(handle, &midiHdr, sizeof(MIDIHDR));
		if (!err)
		{
			// Queue MIDI input buffer
			err = midiInAddBuffer(handle, &midiHdr, sizeof(MIDIHDR));
			if (!err)
			{
				// Start recording Midi
				err = midiInStart(handle);
				if (!err)
				{
					// Check if mapping is requested
					if (mapInProcess)
					{
						mapNotesToKeyboardKeys();
					}

					// Wait for user to abort recording
					printNormalUsageProcessInfo();
					processKeypressing();
					
					/* We need to set a flag to tell our callback midiCallback()
					not to do any more midiInAddBuffer(), because when we
					call midiInReset() below, Windows will send a final
					MIM_LONGDATA message to that callback. If we were to
					allow midiCallback() to midiInAddBuffer() again, we'd
					never get the driver to finish with our midiHdr
					*/
					SysXFlag |= 0x80;
				}

				// Stop recording
				midiInReset(handle);
			}
		}
		
		// If there was an error above, then print a message
		if (err)
		{
			printMidiInError("usage", err);
		}
		
		// Close the MIDI In device
		while ((err = midiInClose(handle)) == MIDIERR_STILLPLAYING)
		{
			Sleep(0);
		}

		if (err)
		{
			printMidiInError("close", err);
		}

		// Unprepare the buffer and MIDIHDR. Unpreparing a buffer that has not been prepared is ok
		midiInUnprepareHeader(handle, &midiHdr, sizeof(MIDIHDR));
	}
	else
	{
		printMidiInError("open", err);
	}

	return 0;
}