#include "stdafx.h"
#include "Thermosmart.h"
#include "../main/Helper.h"
#include "../main/Logger.h"
#include "hardwaretypes.h"
#include "../main/localtime_r.h"
#include "../json/json.h"
#include "../main/RFXtrx.h"
#include "../main/SQLHelper.h"
#include "../httpclient/HTTPClient.h"
#include "../main/mainworker.h"
#include "../json/json.h"

#define round(a) ( int ) ( a + .5 )

const std::string THERMOSMART_LOGIN_PATH = "https://api.thermosmart.com/login";
const std::string THERMOSMART_AUTHORISE_PATH = "https://api.thermosmart.com/oauth2/authorize?response_type=code&client_id=client123&redirect_uri=http://clientapp.com/done";
const std::string THERMOSMART_DECISION_PATH = "https://api.thermosmart.com/oauth2/authorize/decision";
const std::string THERMOSMART_TOKEN_PATH = "https://username:password@api.thermosmart.com/oauth2/token";
const std::string THERMOSMART_ACCESS_PATH = "https://api.thermosmart.com/thermostat/[TID]?access_token=[access_token]";
const std::string THERMOSMART_SETPOINT_PATH = "https://api.thermosmart.com/thermostat/[TID]?access_token=[access_token]";

#ifdef _DEBUG
	//#define DEBUG_ThermosmartThermostat
#endif

#ifdef DEBUG_ThermosmartThermostat
void SaveString2Disk(std::string str, std::string filename)
{
	FILE *fOut = fopen(filename.c_str(), "wb+");
	if (fOut)
	{
		fwrite(str.c_str(), 1, str.size(), fOut);
		fclose(fOut);
	}
}
std::string ReadFile(std::string filename)
{
	std::ifstream file;
	std::string sResult = "";
	file.open(filename.c_str());
	if (!file.is_open())
		return "";
	std::string sLine;
	while (!file.eof())
	{
		getline(file, sLine);
		sResult += sLine;
	}
	file.close();
	return sResult;
}
#endif

CThermosmart::CThermosmart(const int ID, const std::string &Username, const std::string &Password)
{
	m_UserName = "";
	if ((Password == "secret")|| (Password.empty()))
	{
		_log.Log(LOG_ERROR, "Thermosmart: Please update your username/password!...");
	}
	else
	{
		m_UserName = Username;
		m_Password = Password;
		stdstring_trim(m_UserName);
		stdstring_trim(m_Password);
	}
	m_HwdID=ID;
	Init();
}

CThermosmart::~CThermosmart(void)
{
}

void CThermosmart::Init()
{
	m_AccessToken = "";
	m_ThermostatID = "";
	m_stoprequested = false;
	m_bDoLogin = true;
}

bool CThermosmart::StartHardware()
{
	Init();
	m_LastMinute = -1;
	//Start worker thread
	m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&CThermosmart::Do_Work, this)));
	m_bIsStarted=true;
	sOnConnected(this);
	return (m_thread!=NULL);
}

bool CThermosmart::StopHardware()
{
	if (m_thread!=NULL)
	{
		assert(m_thread);
		m_stoprequested = true;
		m_thread->join();
	}
    m_bIsStarted=false;
	if (!m_bDoLogin)
		Logout();
    return true;
}

#define THERMOSMART_POLL_INTERVAL 30

void CThermosmart::Do_Work()
{
	_log.Log(LOG_STATUS,"Thermosmart: Worker started...");
	int sec_counter = THERMOSMART_POLL_INTERVAL-5;
	while (!m_stoprequested)
	{
		sleep_seconds(1);
		sec_counter++;
		if (sec_counter % 12 == 0) {
			m_LastHeartbeat=mytime(NULL);
		}
		if (sec_counter % THERMOSMART_POLL_INTERVAL == 0)
		{
			GetMeterDetails();
		}
	}
	_log.Log(LOG_STATUS,"Thermosmart: Worker stopped...");
}

void CThermosmart::SendSetPointSensor(const unsigned char Idx, const float Temp, const std::string &defaultname)
{
	bool bDeviceExits=true;

	char szID[10];
	sprintf(szID,"%X%02X%02X%02X", 0, 0, 0, Idx);

	std::vector<std::vector<std::string> > result;
	result=m_sql.safe_query("SELECT Name FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q')", m_HwdID, szID);
	if (result.size()<1)
	{
		bDeviceExits=false;
	}

	_tThermostat thermos;
	thermos.subtype=sTypeThermSetpoint;
	thermos.id1=0;
	thermos.id2=0;
	thermos.id3=0;
	thermos.id4=Idx;
	thermos.dunit=0;

	thermos.temp=Temp;

	sDecodeRXMessage(this, (const unsigned char *)&thermos);

	if (!bDeviceExits)
	{
		//Assign default name for device
		result=m_sql.safe_query("UPDATE DeviceStatus SET Name='%q' WHERE (HardwareID==%d) AND (DeviceID=='%q')", defaultname.c_str(), m_HwdID, szID);
	}
}


bool CThermosmart::Login()
{
	if (!m_AccessToken.empty())
	{
		Logout();
	}
	if (m_UserName.empty())
		return false;
	m_AccessToken = "";
	m_ThermostatID = "";

	std::string sURL;
	std::stringstream sstr;
	sstr << "username=" << m_UserName << "&password=" << m_Password;
	std::string szPostdata=sstr.str();
	std::vector<std::string> ExtraHeaders;
	std::string sResult;

	//# 1. Login

	sURL = THERMOSMART_LOGIN_PATH;
	if (!HTTPClient::POST(sURL, szPostdata, ExtraHeaders, sResult))
		{
			_log.Log(LOG_ERROR,"Thermosmart: Error login!");
			return false;
		}

#ifdef DEBUG_ThermosmartThermostat
	SaveString2Disk(sResult, "E:\\thermosmart1.txt");
#endif

	//# 2. Get Authorize Dialog
	sURL = THERMOSMART_AUTHORISE_PATH;
	stdreplace(sURL, "client123", "api-rob-b130d8f5123bf24b");
	ExtraHeaders.clear();
	if (!HTTPClient::GET(sURL, sResult))
	{
		_log.Log(LOG_ERROR, "Thermosmart: Error login!");
		return false;
	}

#ifdef DEBUG_ThermosmartThermostat
	SaveString2Disk(sResult, "E:\\thermosmart2.txt");
#endif

	size_t tpos = sResult.find("value=");
	if (tpos == std::string::npos)
	{
		_log.Log(LOG_ERROR, "Thermosmart: Error login!, check username/password");
		return false;
	}
	sResult = sResult.substr(tpos + 7);
	tpos = sResult.find("\">");
	if (tpos == std::string::npos)
	{
		_log.Log(LOG_ERROR, "Thermosmart: Error login!, check username/password");
		return false;
	}
	std::string TID = sResult.substr(0, tpos);

	//# 3. Authorize  (read out transaction_id from the HTML form received in the previous step). transaction_id prevents from XSRF attacks.
	szPostdata = "transaction_id=" + TID;
	ExtraHeaders.clear();
	sURL = THERMOSMART_DECISION_PATH;
	if (!HTTPClient::POST(sURL, szPostdata, ExtraHeaders, sResult, false))
	{
		_log.Log(LOG_ERROR, "Thermosmart: Error login!, check username/password");
		return false;
	}

#ifdef DEBUG_ThermosmartThermostat
	SaveString2Disk(sResult, "E:\\thermosmart3.txt");
#endif

	tpos = sResult.find("code=");
	if (tpos == std::string::npos)
	{
		_log.Log(LOG_ERROR, "Thermosmart: Error login!, check username/password");
		return false;
	}
	std::string CODE = sResult.substr(tpos + 5);

	//# 4. Exchange authorization code for Access token (read out the code from the previous response)
	szPostdata = "grant_type=authorization_code&code=" + CODE + "&redirect_uri=http://clientapp.com/done";
	sURL = THERMOSMART_TOKEN_PATH;

	stdreplace(sURL, "username", "api-rob-b130d8f5123bf24b");
	stdreplace(sURL, "password", "c1d91661eef0bc4fa2ac67fd");

	if (!HTTPClient::POST(sURL, szPostdata, ExtraHeaders, sResult, false))
	{
		_log.Log(LOG_ERROR, "Thermosmart: Error login!, check username/password");
		return false;
	}

#ifdef DEBUG_ThermosmartThermostat
	SaveString2Disk(sResult, "E:\\thermosmart4.txt");
#endif

	Json::Value root;
	Json::Reader jReader;
	bool ret = jReader.parse(sResult, root);
	if (!ret)
	{
		_log.Log(LOG_ERROR, "Thermosmart: Invalid/no data received...");
		return false;
	}

	if (root["access_token"].empty()||root["thermostat"].empty())
	{
		_log.Log(LOG_ERROR, "Thermosmart: No access granted, check username/password...");
		return false;
	}

	m_AccessToken = root["access_token"].asString();
	m_ThermostatID = root["thermostat"].asString();

	_log.Log(LOG_STATUS, "Thermosmart: Login successfull!...");

	m_bDoLogin = false;
	return true;
}

void CThermosmart::Logout()
{
	if (m_bDoLogin)
		return; //we are not logged in
	m_AccessToken = "";
	m_ThermostatID = "";
	m_bDoLogin = true;
}


bool CThermosmart::WriteToHardware(const char *pdata, const unsigned char length)
{
	if (m_UserName.size() == 0)
		return false;
	if (m_Password.size() == 0)
		return false;

	tRBUF *pCmd = (tRBUF *)pdata;
	if (pCmd->LIGHTING2.packettype != pTypeLighting2)
		return false; //later add RGB support, if someone can provide access

	int node_id = pCmd->LIGHTING2.id4;

	bool bIsOn = (pCmd->LIGHTING2.cmnd == light2_sOn);

	return false;
}

void CThermosmart::GetMeterDetails()
{
	if (m_UserName.size()==0)
		return;
	if (m_Password.size()==0)
		return;
	std::string sResult;
/*
	sResult = ReadFile("E:\\thermosmart_getdata.txt");
*/
	if (m_bDoLogin)
	{
		if (!Login())
			return;
	}
	std::string sURL = THERMOSMART_ACCESS_PATH;
	stdreplace(sURL, "[TID]", m_ThermostatID);
	stdreplace(sURL, "[access_token]", m_AccessToken);
	if (!HTTPClient::GET(sURL, sResult))
	{
		_log.Log(LOG_ERROR, "Thermosmart: Error getting thermostat data!");
		m_bDoLogin = true;
		return;
	}

#ifdef DEBUG_ThermosmartThermostat
	SaveString2Disk(sResult, "E:\\thermosmart_getdata.txt");
#endif

	Json::Value root;
	Json::Reader jReader;
	bool ret = jReader.parse(sResult, root);
	if (!ret)
	{
		_log.Log(LOG_ERROR, "Thermosmart: Invalid/no data received...");
		m_bDoLogin = true;
		return;
	}

	if (root["target_temperature"].empty() || root["room_temperature"].empty())
	{
		_log.Log(LOG_ERROR, "Thermosmart: Invalid/no data received...");
		m_bDoLogin = true;
		return;
	}

	float temperature;
	temperature = (float)root["target_temperature"].asFloat();
	SendSetPointSensor(1, temperature, "target temperature");

	temperature = (float)root["room_temperature"].asFloat();
	SendTempSensor(2, 255, temperature, "room temperature");

	if (!root["outside_temperature"].empty())
	{
		temperature = (float)root["outside_temperature"].asFloat();
		SendTempSensor(3, 255, temperature, "outside temperature");
	}
}

void CThermosmart::SetSetpoint(const int idx, const float temp)
{
	if (m_bDoLogin)
	{
		if (!Login())
			return;
	}

	char szTemp[20];
	sprintf(szTemp, "%.1f", temp);
	std::string sTemp = szTemp;

	std::string szPostdata = "target_temperature=" + sTemp;
	std::vector<std::string> ExtraHeaders;
	std::string sResult;

	std::string sURL = THERMOSMART_SETPOINT_PATH;
	stdreplace(sURL, "[TID]", m_ThermostatID);
	stdreplace(sURL, "[access_token]", m_AccessToken);
	if (!HTTPClient::PUT(sURL, szPostdata, ExtraHeaders, sResult))
	{
		_log.Log(LOG_ERROR, "Thermosmart: Error setting thermostat data!");
		m_bDoLogin = true;
		return;
	}
	SendSetPointSensor(1, temp, "target temperature");
}
