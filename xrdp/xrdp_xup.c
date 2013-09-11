/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2004-2013
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * libxup main file
 */

#include "xrdp_xup.h"

#include "os_calls.h"
#include "defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/shm.h>
#include <sys/stat.h>

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/stream.h>

#include <freerdp/freerdp.h>

int lib_send_all(xrdpModule* mod, unsigned char *data, int len);

static int lib_send_capabilities(xrdpModule* mod)
{
	wStream* s;
	int length;
	rdpSettings* settings;
	XRDP_MSG_CAPABILITIES msg;

	s = mod->SendStream;
	Stream_SetPosition(s, 0);

	msg.msgFlags = 0;
	msg.type = XRDP_CLIENT_CAPABILITIES;

	settings = mod->settings;

	msg.DesktopWidth = settings->DesktopWidth;
	msg.DesktopHeight= settings->DesktopHeight;
	msg.ColorDepth = settings->ColorDepth;

	length = xrdp_write_capabilities(NULL, &msg);

	xrdp_write_capabilities(s, &msg);

	lib_send_all(mod, Stream_Buffer(s), length);

	return 0;
}

int lib_recv(xrdpModule* mod, BYTE* data, int length)
{
	int status;

	if (mod->sck_closed)
		return -1;

	status = recv(mod->sck, data, length, 0);

	if (status < 0)
	{
		if (g_tcp_last_error_would_block(mod->sck))
		{
			if (mod->server->IsTerminated(mod))
			{
				return -1;
			}
		}
		else
		{
			return -1;
		}
	}
	else if (status == 0)
	{
		mod->sck_closed = 1;
		return -1;
	}

	return status;
}

int lib_send_all(xrdpModule* mod, unsigned char *data, int len)
{
	int sent;

	if (mod->sck_closed)
	{
		return 1;
	}

	while (len > 0)
	{
		sent = g_tcp_send(mod->sck, data, len, 0);

		if (sent == -1)
		{
			if (g_tcp_last_error_would_block(mod->sck))
			{
				if (mod->server->IsTerminated(mod))
				{
					return 1;
				}

				g_tcp_can_send(mod->sck, 10);
			}
			else
			{
				return 1;
			}
		}
		else if (sent == 0)
		{
			mod->sck_closed = 1;
			return 1;
		}
		else
		{
			data += sent;
			len -= sent;
		}
	}

	return 0;
}

int x11rdp_xrdp_client_start(xrdpModule* mod, int w, int h, int bpp)
{
	mod->width = w;
	mod->height = h;
	mod->bpp = bpp;
	return 0;
}

int x11rdp_xrdp_client_connect(xrdpModule* mod)
{
	int i;
	int index;
	int status;
	int length;
	int use_uds;
	wStream* s;
	char con_port[256];
	XRDP_MSG_OPAQUE_RECT opaqueRect;
	XRDP_MSG_BEGIN_UPDATE beginUpdate;
	XRDP_MSG_END_UPDATE endUpdate;

	beginUpdate.type = XRDP_SERVER_BEGIN_UPDATE;
	endUpdate.type = XRDP_SERVER_END_UPDATE;

	opaqueRect.type = XRDP_SERVER_OPAQUE_RECT;
	opaqueRect.nTopRect = 0;
	opaqueRect.nLeftRect = 0;
	opaqueRect.nWidth = mod->width;
	opaqueRect.nHeight = mod->height;
	opaqueRect.color = 0;

	/* clear screen */

	mod->server->BeginUpdate(mod, &beginUpdate);
	mod->server->OpaqueRect(mod, &opaqueRect);
	mod->server->EndUpdate(mod, &endUpdate);

	/* only support 8, 15, 16, and 24 bpp connections from rdp client */
	if (mod->bpp != 8 && mod->bpp != 15 && mod->bpp != 16 && mod->bpp != 24 && mod->bpp != 32)
	{
		LIB_DEBUG(mod, "x11rdp_xrdp_client_connect error");
		return 1;
	}

	if (g_strcmp(mod->ip, "") == 0)
	{
		LIB_DEBUG(mod, "x11rdp_xrdp_client_connect error");
		return 1;
	}

	g_sprintf(con_port, "%s", mod->port);
	use_uds = 0;

	if (con_port[0] == '/')
	{
		use_uds = 1;
	}

	mod->sck_closed = 0;
	i = 0;

	while (1)
	{
		if (use_uds)
		{
			mod->sck = g_tcp_local_socket();
		}
		else
		{
			mod->sck = g_tcp_socket();
			g_tcp_set_non_blocking(mod->sck);
			g_tcp_set_no_delay(mod->sck);
		}

		if (use_uds)
		{
			status = g_tcp_local_connect(mod->sck, con_port);
		}
		else
		{
			status = g_tcp_connect(mod->sck, mod->ip, con_port);
		}

		if (status == -1)
		{
			if (g_tcp_last_error_would_block(mod->sck))
			{
				status = 0;
				index = 0;

				while (!g_tcp_can_send(mod->sck, 100))
				{
					index++;

					if ((index >= 30) || mod->server->IsTerminated(mod))
					{
						status = 1;
						break;
					}
				}
			}
			else
			{

			}
		}

		if (status == 0)
		{
			break;
		}

		g_tcp_close(mod->sck);
		mod->sck = 0;
		i++;

		if (i >= 4)
		{

			break;
		}

		g_sleep(250);
	}

	lib_send_capabilities(mod);

	if (status == 0)
	{
		RECTANGLE_16 rect;
		XRDP_MSG_REFRESH_RECT msg;

		msg.msgFlags = 0;
		msg.type = XRDP_CLIENT_REFRESH_RECT;

		msg.numberOfAreas = 1;
		msg.areasToRefresh = &rect;

		rect.left = 0;
		rect.top = 0;
		rect.right = mod->settings->DesktopWidth - 1;
		rect.bottom = mod->settings->DesktopHeight - 1;

		s = mod->SendStream;
		Stream_SetPosition(s, 0);

		length = xrdp_write_refresh_rect(NULL, &msg);

		xrdp_write_refresh_rect(s, &msg);

		lib_send_all(mod, Stream_Buffer(s), length);
	}

	if (status != 0)
	{
		LIB_DEBUG(mod, "x11rdp_xrdp_client_connect error");
		return 1;
	}
	else
	{
		mod->SocketEvent = CreateFileDescriptorEvent(NULL, TRUE, FALSE, mod->sck);
		ResumeThread(mod->ServerThread);
	}

	return 0;
}

int x11rdp_xrdp_client_event(xrdpModule* mod, int subtype, long param1, long param2, long param3, long param4)
{
	wStream* s;
	int length;
	int key;
	int status;
	XRDP_MSG_EVENT msg;

	LIB_DEBUG(mod, "in lib_mod_event");

	s = mod->SendStream;
	Stream_SetPosition(s, 0);

	if ((subtype >= 15) && (subtype <= 16)) /* key events */
	{
		key = param2;

		if (key > 0)
		{
			if (key == 65027) /* altgr */
			{
				if (mod->shift_state)
				{
					g_writeln("special");
					/* fix for mstsc sending left control down with altgr */
					/* control down / up
					 msg param1 param2 param3 param4
					 15  0      65507  29     0
					 16  0      65507  29     49152 */

					msg.msgFlags = 0;
					msg.type = XRDP_CLIENT_EVENT;

					msg.subType = 16; /* key up */
					msg.param1 = 0;
					msg.param2 = 65507; /* left control */
					msg.param3 = 29; /* RDP scan code */
					msg.param4 = 0xc000; /* flags */

					Stream_SetPosition(s, 0);

					length = xrdp_write_event(NULL, &msg);
					xrdp_write_event(s, &msg);

					status = lib_send_all(mod, Stream_Buffer(s), length);
				}
			}

			if (key == 65507) /* left control */
			{
				mod->shift_state = subtype == 15;
			}
		}
	}

	msg.msgFlags = 0;
	msg.type = XRDP_CLIENT_EVENT;

	msg.subType = subtype;
	msg.param1 = param1;
	msg.param2 = param2;
	msg.param3 = param3;
	msg.param4 = param4;

	Stream_SetPosition(s, 0);

	length = xrdp_write_event(NULL, &msg);
	xrdp_write_event(s, &msg);

	status = lib_send_all(mod, Stream_Buffer(s), length);

	return status;
}

int x11rdp_xrdp_client_synchronize_keyboard_event(xrdpModule* mod, DWORD flags)
{
	int length;
	int status;
	wStream* s;
	XRDP_MSG_SYNCHRONIZE_KEYBOARD_EVENT msg;

	msg.msgFlags = 0;
	msg.type = XRDP_CLIENT_SYNCHRONIZE_KEYBOARD_EVENT;

	msg.flags = flags;

	s = mod->SendStream;
	Stream_SetPosition(s, 0);

	length = xrdp_write_synchronize_keyboard_event(NULL, &msg);
	xrdp_write_synchronize_keyboard_event(s, &msg);

	status = lib_send_all(mod, Stream_Buffer(s), length);

	return status;
}

int x11rdp_xrdp_client_scancode_keyboard_event(xrdpModule* mod, DWORD flags, DWORD code, DWORD keyboardType)
{
	int length;
	int status;
	wStream* s;
	XRDP_MSG_SCANCODE_KEYBOARD_EVENT msg;

	msg.msgFlags = 0;
	msg.type = XRDP_CLIENT_SCANCODE_KEYBOARD_EVENT;

	msg.flags = flags;
	msg.code = code;
	msg.keyboardType = keyboardType;

	s = mod->SendStream;
	Stream_SetPosition(s, 0);

	length = xrdp_write_scancode_keyboard_event(NULL, &msg);
	xrdp_write_scancode_keyboard_event(s, &msg);

	status = lib_send_all(mod, Stream_Buffer(s), length);

	return status;
}

int x11rdp_xrdp_client_virtual_keyboard_event(xrdpModule* mod, DWORD flags, DWORD code)
{
	int length;
	int status;
	wStream* s;
	XRDP_MSG_VIRTUAL_KEYBOARD_EVENT msg;

	msg.msgFlags = 0;
	msg.type = XRDP_CLIENT_VIRTUAL_KEYBOARD_EVENT;

	msg.flags = flags;
	msg.code = code;

	s = mod->SendStream;
	Stream_SetPosition(s, 0);

	length = xrdp_write_virtual_keyboard_event(NULL, &msg);
	xrdp_write_virtual_keyboard_event(s, &msg);

	status = lib_send_all(mod, Stream_Buffer(s), length);

	return status;
}

int x11rdp_xrdp_client_unicode_keyboard_event(xrdpModule* mod, DWORD flags, DWORD code)
{
	int length;
	int status;
	wStream* s;
	XRDP_MSG_UNICODE_KEYBOARD_EVENT msg;

	msg.msgFlags = 0;
	msg.type = XRDP_CLIENT_UNICODE_KEYBOARD_EVENT;

	msg.flags = flags;
	msg.code = code;

	s = mod->SendStream;
	Stream_SetPosition(s, 0);

	length = xrdp_write_unicode_keyboard_event(NULL, &msg);
	xrdp_write_unicode_keyboard_event(s, &msg);

	status = lib_send_all(mod, Stream_Buffer(s), length);

	return status;
}

int x11rdp_xrdp_client_mouse_event(xrdpModule* mod, DWORD flags, DWORD x, DWORD y)
{
	int length;
	int status;
	wStream* s;
	XRDP_MSG_MOUSE_EVENT msg;

	msg.msgFlags = 0;
	msg.type = XRDP_CLIENT_MOUSE_EVENT;

	msg.flags = flags;
	msg.x = x;
	msg.y = y;

	s = mod->SendStream;
	Stream_SetPosition(s, 0);

	length = xrdp_write_mouse_event(NULL, &msg);
	xrdp_write_mouse_event(s, &msg);

	status = lib_send_all(mod, Stream_Buffer(s), length);

	return status;
}

int x11rdp_xrdp_client_extended_mouse_event(xrdpModule* mod, DWORD flags, DWORD x, DWORD y)
{
	int length;
	int status;
	wStream* s;
	XRDP_MSG_EXTENDED_MOUSE_EVENT msg;

	msg.msgFlags = 0;
	msg.type = XRDP_CLIENT_EXTENDED_MOUSE_EVENT;

	msg.flags = flags;
	msg.x = x;
	msg.y = y;

	s = mod->SendStream;
	Stream_SetPosition(s, 0);

	length = xrdp_write_extended_mouse_event(NULL, &msg);
	xrdp_write_extended_mouse_event(s, &msg);

	status = lib_send_all(mod, Stream_Buffer(s), length);

	return status;
}

int xup_recv_msg(xrdpModule* mod, wStream* s, XRDP_MSG_COMMON* common)
{
	int status = 0;

	switch (common->type)
	{
		case XRDP_SERVER_BEGIN_UPDATE:
			{
				XRDP_MSG_BEGIN_UPDATE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->BeginUpdate(mod, &msg);
			}
			break;

		case XRDP_SERVER_END_UPDATE:
			{
				XRDP_MSG_END_UPDATE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->EndUpdate(mod, &msg);
			}
			break;

		case XRDP_SERVER_OPAQUE_RECT:
			{
				XRDP_MSG_OPAQUE_RECT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->OpaqueRect(mod, &msg);
			}
			break;

		case XRDP_SERVER_SCREEN_BLT:
			{
				XRDP_MSG_SCREEN_BLT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->ScreenBlt(mod, &msg);
			}
			break;

		case XRDP_SERVER_PATBLT:
			{
				XRDP_MSG_PATBLT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->PatBlt(mod, &msg);
			}
			break;

		case XRDP_SERVER_DSTBLT:
			{
				XRDP_MSG_DSTBLT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->DstBlt(mod, &msg);
			}
			break;

		case XRDP_SERVER_PAINT_RECT:
			{
				int status;
				XRDP_MSG_PAINT_RECT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));

				msg.fbSegmentId = 0;
				msg.framebuffer = NULL;

				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);

				if (msg.fbSegmentId)
					msg.framebuffer = &(mod->framebuffer);

				status = mod->server->PaintRect(mod, &msg);
			}
			break;

		case XRDP_SERVER_SET_CLIPPING_REGION:
			{
				XRDP_MSG_SET_CLIPPING_REGION msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->SetClippingRegion(mod, &msg);
			}
			break;

		case XRDP_SERVER_LINE_TO:
			{
				XRDP_MSG_LINE_TO msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->LineTo(mod, &msg);
			}
			break;

		case XRDP_SERVER_SET_POINTER:
			{
				XRDP_MSG_SET_POINTER msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->SetPointer(mod, &msg);
			}
			break;

		case XRDP_SERVER_CREATE_OFFSCREEN_SURFACE:
			{
				XRDP_MSG_CREATE_OFFSCREEN_SURFACE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->CreateOffscreenSurface(mod, &msg);
			}
			break;

		case XRDP_SERVER_SWITCH_OFFSCREEN_SURFACE:
			{
				XRDP_MSG_SWITCH_OFFSCREEN_SURFACE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->SwitchOffscreenSurface(mod, &msg);
			}
			break;

		case XRDP_SERVER_DELETE_OFFSCREEN_SURFACE:
			{
				XRDP_MSG_DELETE_OFFSCREEN_SURFACE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->DeleteOffscreenSurface(mod, &msg);
			}
			break;

		case XRDP_SERVER_PAINT_OFFSCREEN_SURFACE:
			{
				XRDP_MSG_PAINT_OFFSCREEN_SURFACE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->PaintOffscreenSurface(mod, &msg);
			}
			break;

		case XRDP_SERVER_WINDOW_NEW_UPDATE:
			{
				XRDP_MSG_WINDOW_NEW_UPDATE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->WindowNewUpdate(mod, &msg);
			}
			break;

		case XRDP_SERVER_WINDOW_DELETE:
			{
				XRDP_MSG_WINDOW_DELETE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->WindowDelete(mod, &msg);
			}
			break;

		case XRDP_SERVER_SHARED_FRAMEBUFFER:
			{
				XRDP_MSG_SHARED_FRAMEBUFFER msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = mod->server->SharedFramebuffer(mod, &msg);
			}
			break;

		default:
			g_writeln("lib_mod_process_orders: unknown order type %d", common->type);
			status = 0;
			break;
	}

	return status;
}

int xup_recv(xrdpModule* mod)
{
	wStream* s;
	int index;
	int status;
	int position;

	s = mod->ReceiveStream;

	if (Stream_GetPosition(s) < 8)
	{
		status = lib_recv(mod, Stream_Pointer(s), 8 - Stream_GetPosition(s));

		if (status > 0)
			Stream_Seek(s, status);

		if (Stream_GetPosition(s) >= 8)
		{
			position = Stream_GetPosition(s);
			Stream_SetPosition(s, 0);

			Stream_Read_UINT32(s, mod->TotalLength);
			Stream_Read_UINT32(s, mod->TotalCount);

			Stream_SetPosition(s, position);

			Stream_EnsureCapacity(s, mod->TotalLength);
		}
	}

	if (Stream_GetPosition(s) >= 8)
	{
		status = lib_recv(mod, Stream_Pointer(s), mod->TotalLength - Stream_GetPosition(s));

		if (status > 0)
			Stream_Seek(s, status);
	}

	if (Stream_GetPosition(s) >= mod->TotalLength)
	{
		Stream_SetPosition(s, 8);

		for (index = 0; index < mod->TotalCount; index++)
		{
			XRDP_MSG_COMMON common;

			position = Stream_GetPosition(s);

			xrdp_read_common_header(s, &common);

			status = xup_recv_msg(mod, s, &common);

			if (status != 0)
			{
				break;
			}

			Stream_SetPosition(s, position + common.length);
		}

		Stream_SetPosition(s, 0);
		mod->TotalLength = 0;
		mod->TotalCount = 0;
	}

	return 0;
}

int x11rdp_xrdp_client_end(xrdpModule* mod)
{
	SetEvent(mod->StopEvent);

	return 0;
}

void* x11rdp_xrdp_client_thread(void* arg)
{
	int fps;
	DWORD status;
	DWORD nCount;
	HANDLE events[8];
	HANDLE PackTimer;
	LARGE_INTEGER due;
	xrdpModule* mod = (xrdpModule*) arg;

	fps = mod->fps;
	PackTimer = CreateWaitableTimer(NULL, TRUE, NULL);

	due.QuadPart = 0;
	SetWaitableTimer(PackTimer, &due, 1000 / fps, NULL, NULL, 0);

	nCount = 0;
	events[nCount++] = PackTimer;
	events[nCount++] = mod->StopEvent;
	events[nCount++] = mod->SocketEvent;

	while (1)
	{
		status = WaitForMultipleObjects(nCount, events, FALSE, INFINITE);

		if (WaitForSingleObject(mod->StopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}

		if (WaitForSingleObject(mod->SocketEvent, 0) == WAIT_OBJECT_0)
		{
			xup_recv(mod);
		}

		if (status == WAIT_OBJECT_0)
		{
			xrdp_message_server_queue_pack(mod);
		}

		if (mod->fps != fps)
		{
			fps = mod->fps;
			due.QuadPart = 0;
			SetWaitableTimer(PackTimer, &due, 1000 / fps, NULL, NULL, 0);
		}
	}

	CloseHandle(PackTimer);

	return NULL;
}

int x11rdp_xrdp_client_get_event_handles(xrdpModule* mod, HANDLE* events, DWORD* nCount)
{
	if (mod)
	{
		if (mod->ServerQueue)
		{
			events[*nCount] = MessageQueue_Event(mod->ServerQueue);
			(*nCount)++;
		}
	}

	return 0;
}

int x11rdp_xrdp_client_check_event_handles(xrdpModule* mod)
{
	int status = 0;

	if (!mod)
		return 0;

	while (WaitForSingleObject(MessageQueue_Event(mod->ServerQueue), 0) == WAIT_OBJECT_0)
	{
		status = xrdp_message_server_queue_process_pending_messages(mod);
	}

	return status;
}

int xup_module_init(xrdpModule* mod)
{
	xrdpClientModule* client;

	client = (xrdpClientModule*) malloc(sizeof(xrdpClientModule));
	mod->client = client;

	if (client)
	{
		ZeroMemory(client, sizeof(xrdpClientModule));

		client->Connect = x11rdp_xrdp_client_connect;
		client->Start = x11rdp_xrdp_client_start;
		client->Event = x11rdp_xrdp_client_event;
		client->SynchronizeKeyboardEvent = x11rdp_xrdp_client_synchronize_keyboard_event;
		client->ScancodeKeyboardEvent = x11rdp_xrdp_client_scancode_keyboard_event;
		client->VirtualKeyboardEvent = x11rdp_xrdp_client_virtual_keyboard_event;
		client->UnicodeKeyboardEvent = x11rdp_xrdp_client_unicode_keyboard_event;
		client->MouseEvent = x11rdp_xrdp_client_mouse_event;
		client->ExtendedMouseEvent = x11rdp_xrdp_client_extended_mouse_event;
		client->End = x11rdp_xrdp_client_end;
		client->GetEventHandles = x11rdp_xrdp_client_get_event_handles;
		client->CheckEventHandles = x11rdp_xrdp_client_check_event_handles;
	}

	mod->SendStream = Stream_New(NULL, 8192);
	mod->ReceiveStream = Stream_New(NULL, 8192);

	mod->TotalLength = 0;
	mod->TotalCount = 0;

	mod->StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	mod->ServerThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) x11rdp_xrdp_client_thread,
			(void*) mod, CREATE_SUSPENDED, NULL);

	g_strncpy(mod->ip, "127.0.0.1", 255);

	return 0;
}

int xup_module_exit(xrdpModule* mod)
{
	SetEvent(mod->StopEvent);

	WaitForSingleObject(mod->ServerThread, INFINITE);
	CloseHandle(mod->ServerThread);

	Stream_Free(mod->SendStream, TRUE);
	Stream_Free(mod->ReceiveStream, TRUE);

	CloseHandle(mod->StopEvent);

	g_tcp_close(mod->sck);

	return 0;
}