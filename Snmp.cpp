/* 
 * Implements the SNMP and IPHLPAPI functions.
 */

#include "StdAfx.h"
#include "NetPerSec.h"
#include "winsock2.h"


HANDLE hPollForTrapEvent = NULL;
AsnObjectIdentifier SupportedView = {0, 0};


LPVOID CSnmp::SnmpUtilMemAlloc(UINT nSize) {
	if (m_fpSnmpUtilMemAlloc != NULL)
		return m_fpSnmpUtilMemAlloc(nSize);
	else
		return GlobalAlloc(GPTR, nSize);
}


void CSnmp::SnmpUtilMemFree(LPVOID pMem) {
	if (m_fpSnmpUtilMemFree != NULL)
		m_fpSnmpUtilMemFree(pMem);
	else
		GlobalFree(pMem);
}


CSnmp::CSnmp() {
	m_fpSnmpUtilMemAlloc = NULL;
	m_fpSnmpUtilMemFree = NULL;
	m_pvarBindList = NULL;
	m_bUse_iphlpapi = false;
	m_hInst = NULL;
	m_hInstIpHlp = NULL;
	m_hInstSnmp = NULL;
	m_dwInterfaces = 0;
	m_fpGetNumberOfInterfaces = NULL;
	m_fpGetIfEntry = NULL;
	m_bUseGetInterfaceInfo = false;  // Win2000
}


CSnmp::~CSnmp() {
	if (m_pvarBindList != NULL) SnmpUtilMemFree(m_pvarBindList);
	if (m_hInst      != NULL) FreeLibrary(m_hInst);
	if (m_hInstIpHlp != NULL) FreeLibrary(m_hInstIpHlp);
	if (m_hInstSnmp  != NULL) FreeLibrary(m_hInstSnmp);
}


// If running under NT use iphlpapi - requires SP4 for NT4 otherwise there is a memory leak in SNMP.
// We could use this on Win98, however certain releases of IE5 cause iphlpapi to fail.
bool CSnmp::CheckNT() {
	m_dwInterfaces = 0;
	
	// Check for NT
	OSVERSIONINFO os;
	os.dwOSVersionInfoSize = sizeof(os);
	GetVersionEx(&os);
	
	if (os.dwPlatformId == VER_PLATFORM_WIN32_NT) {
		m_hInstIpHlp = LoadLibraryEx("iphlpapi.dll", NULL, 0);
		
		if (m_hInstIpHlp != NULL) {
			m_fpGetIfEntry = (fpGetIfEntry)GetProcAddress(m_hInstIpHlp, "GetIfEntry");
			m_fpGetNumberOfInterfaces = (fpGetNumberOfInterfaces)GetProcAddress(m_hInstIpHlp, "GetNumberOfInterfaces");
			
			// If WinNT 4
			if (os.dwMajorVersion < 5) {
				// Requires SP4 or higher
				if (GetServicePack() > 0x300 && m_fpGetNumberOfInterfaces != 0)
					m_bUse_iphlpapi = true;
				
			} else {
				// Windows 2000 and Win98 use what appear to be scalar values in the lower word
				// and control flags in the upper word of the adapter[].index .
				// GetInterfaceInfo is not supported by WinNT.
				m_fpGetInterfaceInfo = (fpGetInterfaceInfo)GetProcAddress(m_hInstIpHlp, "GetInterfaceInfo");
				if (m_fpGetInterfaceInfo != NULL) {
					m_bUseGetInterfaceInfo = true;
					m_bUse_iphlpapi = true;
				}
			}
		}
	}
	return m_bUse_iphlpapi;
}


// Check if an interface, such as DUN, has been added or removed.
// Win2000 does not report adapters until they are used.
void CSnmp::GetInterfaces() {
	if (m_bUseGetInterfaceInfo) {
		if (m_fpGetInterfaceInfo != NULL) {
			// Query size
			DWORD dwSize = 0;
			m_fpGetInterfaceInfo(NULL, &dwSize);
			
			if (dwSize != 0) {
				PIP_INTERFACE_INFO pInterface = (PIP_INTERFACE_INFO)GlobalAlloc(GPTR, dwSize);
				if (pInterface != NULL) {
					m_fpGetInterfaceInfo(pInterface, &dwSize);
					m_dwInterfaces = min(MAX_INTERFACES, pInterface->NumAdapters);
					
					for (DWORD i = 0; i < m_dwInterfaces; i++)
						m_dwInterfaceArray[i] = pInterface->Adapter[i].Index;
					
					GlobalFree(pInterface);
				}
			}
		}
	} else {
		// WinNT 4 uses scalar "friendly" values for the GetIfEntry function,
		// although this is poorly documented by Microsoft
		if (m_fpGetNumberOfInterfaces(&m_dwInterfaces) == NO_ERROR) {
			m_dwInterfaces = min(MAX_INTERFACES, m_dwInterfaces);
			for (DWORD i = 0; i < m_dwInterfaces; i++)
				m_dwInterfaceArray[i] = i + 1;  // Not zero-based
		}
	}
}


// Loads the SNMP DLLs.
bool CSnmp::Init() {
	CheckNT();
	
	m_hInst = LoadLibraryEx("inetmib1.dll", NULL, 0);
	if (m_hInst == NULL) {
		ShowSystemError(IDS_INETMIB1_ERR);
		return false;
	}
	
	m_fpExtensionInit = (pSnmpExtensionInit)GetProcAddress(m_hInst, "SnmpExtensionInit");
	m_fpExtensionQuery = (pSnmpExtensionQuery)GetProcAddress(m_hInst, "SnmpExtensionQuery");
	
	if (m_fpExtensionInit == NULL) {
		ShowSystemError(IDS_SNMPINIT_ERR);
		return false;
	}
	if (m_fpExtensionQuery == NULL) {
		ShowSystemError(IDS_SNMPQUERY_ERR);
		return false;
	}
	
	// Init
	if (m_fpExtensionInit(GetTickCount(), &hPollForTrapEvent, &SupportedView) == NULL) {
		ShowSystemError(IDS_SNMPFAIL_ERR);
		return false;
	}
	
	// Check to see if the MemAlloc and MemFree functions are available
	m_hInstSnmp = LoadLibraryEx("snmpapi.dll", NULL, 0);
	
	if (m_hInstSnmp != NULL) {
		m_fpSnmpUtilMemAlloc = (SUALLOC)GetProcAddress(m_hInstSnmp, "SnmpUtilMemAlloc");
		m_fpSnmpUtilMemFree = (SUFREE)GetProcAddress(m_hInstSnmp, "SnmpUtilMemFree");
		m_fpSnmpUtilOidFree = (pSnmpUtilOidFree)GetProcAddress(m_hInstSnmp, "SnmpUtilOidFree");
		m_fpSnmpUtilVarBindFree = (pSnmpUtilVarBindFree)GetProcAddress(m_hInstSnmp, "SnmpUtilVarBindFree");
		m_fpSnmpUtilOidNCmp = (pSnmpUtilOidNCmp)GetProcAddress(m_hInstSnmp, "SnmpUtilOidNCmp");
		m_fpSnmpUtilOidCpy = (pSnmpUtilOidCpy)GetProcAddress(m_hInstSnmp, "SnmpUtilOidCpy");
	} else {
		ShowSystemError(IDS_SNMPAPI_ERR);
		return false;
	}
	
	// Allocate our bindlist
	m_pvarBindList = (SnmpVarBindList*)SnmpUtilMemAlloc(sizeof(SnmpVarBindList));
	ASSERT(m_pvarBindList != NULL);
	
	return true;
}


// Uses the IPHLPAPI interface to retrieve the sent and received bytes
bool CSnmp::GetReceivedAndSentOctets_IPHelper(DWORD &pReceived, DWORD &pSent) {
	MIB_IFROW mib;
	ZeroMemory(&mib, sizeof(mib));
	
	pReceived = 0;
	pSent = 0;
	bool ok = false;
	
	GetInterfaces();
	for (DWORD i = 0; i < m_dwInterfaces; i++) {
		mib.dwIndex = m_dwInterfaceArray[i];
		
		// Monitor specific adapter?
		if (g_MonitorMode == MONITOR_ADAPTER && g_dwAdapter != mib.dwIndex)
			continue;
		
		if (m_fpGetIfEntry(&mib) == NO_ERROR && mib.dwType != MIB_IF_TYPE_LOOPBACK &&
				(mib.dwOperStatus == IF_OPER_STATUS_CONNECTED || mib.dwOperStatus == IF_OPER_STATUS_OPERATIONAL)) {
			pReceived += mib.dwInOctets;
			pSent += mib.dwOutOctets;
			ok = true;
		}
	}
	return ok;
}


// Returns the number of bytes received and sent through all network interfaces
void CSnmp::GetReceivedAndSentOctets_9x(DWORD &pRecv, DWORD &pSent) {
	pRecv = 0;
	pSent = 0;
	
	#define VAR_BINDS 3
	RFC1157VarBind varBind[VAR_BINDS];
	AsnInteger errorStatus;
	AsnInteger errorIndex;
	AsnObjectIdentifier tempOid;
	
	static AsnObjectIdentifier MIB_NULL = {0, 0};
	
	static UINT OID_ifInoctets[] = {1, 3, 6, 1, 2, 1, 2, 2, 1, 10, 0};
	AsnObjectIdentifier MIB_ifInoctets = {ELEMENTS(OID_ifInoctets), OID_ifInoctets};
	
	static UINT OID_ifOutoctets[] = {1, 3, 6, 1, 2, 1, 2, 2, 1, 16, 0};
	AsnObjectIdentifier MIB_ifOutoctets = {ELEMENTS(OID_ifOutoctets), OID_ifOutoctets};
	
	static UINT OID_ifType[] = {1, 3, 6, 1, 2, 1, 2, 2, 1, 3};
	AsnObjectIdentifier MIB_ifType = {ELEMENTS(OID_ifType), OID_ifType};
	
	ASSERT(m_pvarBindList != NULL);
	
	m_pvarBindList->list = varBind;
	m_pvarBindList->len = VAR_BINDS;
	varBind[0].name = MIB_NULL;
	varBind[1].name = MIB_NULL;
	varBind[2].name = MIB_NULL;
	
	// Monitor specific adapter?
	if (g_MonitorMode == MONITOR_ADAPTER) {
		m_pvarBindList->len = 2;
		OID_ifInoctets[10] = g_dwAdapter;
		OID_ifOutoctets[10] = g_dwAdapter;
		
		m_fpSnmpUtilOidCpy(&varBind[0].name, &MIB_ifInoctets);
		m_fpSnmpUtilOidCpy(&varBind[1].name, &MIB_ifOutoctets);
		
		int ret = m_fpExtensionQuery(ASN_RFC1157_GETREQUEST, m_pvarBindList, &errorStatus, &errorIndex);
		if (ret != 0 && errorStatus == 0) {
			pRecv = varBind[0].value.asnValue.number;
			pSent = varBind[1].value.asnValue.number;
		}
		
		m_fpSnmpUtilOidFree(&varBind[0].name);
		m_fpSnmpUtilOidFree(&varBind[1].name);
		return;
	}
	
	// Monitor all adapters
	OID_ifInoctets[10] = 0;
	OID_ifOutoctets[10] = 0;
	
	m_fpSnmpUtilOidCpy(&varBind[0].name, &MIB_ifInoctets);
	m_fpSnmpUtilOidCpy(&varBind[1].name, &MIB_ifOutoctets);
	m_fpSnmpUtilOidCpy(&varBind[2].name, &MIB_ifType);
	
	while (true) {
		int ret = m_fpExtensionQuery(ASN_RFC1157_GETNEXTREQUEST, m_pvarBindList, &errorStatus, &errorIndex);
		if (ret == 0)
			break;
		
		ret = m_fpSnmpUtilOidNCmp(&varBind[0].name, &MIB_ifInoctets, MIB_ifInoctets.idLength - 1);
		if (ret != 0)
			break;
		
		if (varBind[2].value.asnValue.number != MIB_IF_TYPE_LOOPBACK) {
			pRecv += varBind[0].value.asnValue.number;
			pSent += varBind[1].value.asnValue.number;
		}
		
		// Prepare for the next iteration. Make sure returned oid is
		// preserved and the returned value is freed.
		for (int i = 0; i < VAR_BINDS; i++) {
			m_fpSnmpUtilOidCpy(&tempOid, &varBind[i].name);
			m_fpSnmpUtilVarBindFree(&varBind[i]);
			m_fpSnmpUtilOidCpy(&varBind[i].name, &tempOid);
			varBind[i].value.asnType = ASN_NULL;
			m_fpSnmpUtilOidFree(&tempOid);
		}
	}
	
	for (int i = 0; i < VAR_BINDS; i++)
		m_fpSnmpUtilOidFree(&varBind[i].name);
}


// Returns a list of adapter names and index values
void CSnmp::GetInterfaceDescriptions(CStringArray *sArray, CUIntArray *nAdapter) {
	#define VAR_BINDS_DESCRIPTIONS 3
	AsnInteger errorStatus;
	AsnInteger errorIndex;
	AsnObjectIdentifier tempOid;
	SnmpVarBindList varBindList;
	RFC1157VarBind varBind[VAR_BINDS_DESCRIPTIONS];
	
	static AsnObjectIdentifier MIB_NULL = {0, 0};
	
	static UINT OID_ifDesc[] = {1, 3, 6, 1, 2, 1, 2, 2, 1, 2};
	AsnObjectIdentifier MIB_ifDesc = {ELEMENTS(OID_ifDesc), OID_ifDesc};
	
	static UINT OID_ifIndex[] = {1, 3, 6, 1, 2, 1, 2, 2, 1, 1};
	AsnObjectIdentifier MIB_ifIndex = {ELEMENTS(OID_ifIndex), OID_ifIndex};
	
	static UINT OID_ifType[] = {1, 3, 6, 1, 2, 1, 2, 2, 1, 3};
	AsnObjectIdentifier MIB_ifType = {ELEMENTS(OID_ifType), OID_ifType};
	
	varBindList.list = varBind;
	varBindList.len = VAR_BINDS_DESCRIPTIONS;
	varBind[0].name = MIB_NULL;
	varBind[1].name = MIB_NULL;
	varBind[2].name = MIB_NULL;
	
	m_fpSnmpUtilOidCpy(&varBind[0].name, &MIB_ifDesc);
	m_fpSnmpUtilOidCpy(&varBind[1].name, &MIB_ifIndex);
	m_fpSnmpUtilOidCpy(&varBind[2].name, &MIB_ifType);
	
	while (true) {
		if (m_fpExtensionQuery(ASN_RFC1157_GETNEXTREQUEST, &varBindList, &errorStatus, &errorIndex) == 0)
			break;
		if (m_fpSnmpUtilOidNCmp(&varBind[0].name, &MIB_ifDesc, MIB_ifDesc.idLength) != 0)
			break;
		
		// Win9x occasionally fails to truncate the ifDesc string (and leaks memory when this happens).
		// Limit the output string to 32 characters max
		if (errorStatus == 0 && varBind[2].value.asnValue.number != MIB_IF_TYPE_LOOPBACK) {
			char s[32];
			strncpy_s(s, (char*)varBind[0].value.asnValue.string.stream, _TRUNCATE);
			sArray->Add(s);
			nAdapter->Add(varBind[1].value.asnValue.number);
		}
		
		for (int i = 0; i < VAR_BINDS_DESCRIPTIONS; i++) {
			// Prepare for the next iteration. Make sure returned oid is
			// preserved and the returned value is freed.
			m_fpSnmpUtilOidCpy(&tempOid, &varBind[i].name);
			m_fpSnmpUtilVarBindFree(&varBind[i]);
			m_fpSnmpUtilOidCpy(&varBind[i].name, &tempOid);
			varBind[i].value.asnType = ASN_NULL;
			m_fpSnmpUtilOidFree(&tempOid);
		}
	}
	
	for (int i = 0; i < VAR_BINDS_DESCRIPTIONS; i++)
		m_fpSnmpUtilOidFree(&varBind[i].name);
}


void CSnmp::ShowSystemError(int nID) {
	DWORD dwErr = GetLastError();
	
	LPVOID lpMsgBuf;
	DWORD rc = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, dwErr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&lpMsgBuf, 0, NULL);
	
	CString sMsg;
	sMsg.LoadString(nID);
	CString sErr;
	sErr.Format("%s\n\nError code = %u.  %s\nPlease review the Troubleshooting section in the online help.", sMsg, dwErr, lpMsgBuf);
	AfxMessageBox(sErr, MB_OK | MB_ICONHAND | MB_SETFOREGROUND);
	LocalFree(lpMsgBuf);
}


// Returns the number of bytes received and sent
bool CSnmp::GetReceivedAndSentOctets(DWORD &pRecv, DWORD &pSent) {
	bool ok = true;  // Default value
	if (g_MonitorMode == MONITOR_DUN)  // Use performance data from the registry
		perfdata.GetReceivedAndSentOctets(pRecv, pSent);
	else if (m_bUse_iphlpapi)  // Use IPHLPAPI.DLL
		ok = GetReceivedAndSentOctets_IPHelper(pRecv, pSent);
	else  // Use INETMIB1.DLL
		GetReceivedAndSentOctets_9x(pRecv, pSent);
	return ok;
}
