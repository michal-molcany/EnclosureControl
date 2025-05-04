class secrets
{
public:
    String getWiFiSSID();
    String getWiFiPassword();
    String getOctoprintApiKey();
};

String secrets::getWiFiSSID()
{
    return "WIFI SSID";
}

String secrets::getWiFiPassword()
{
    return "WIFI PASSWORD";
}

String secrets::getOctoprintApiKey()
{
    return "OCTOPRINT API KEY";
}
#define OCTOTPRINT_IP "192.168.1.X" // IP address of the OctoPrint server (change this to your OctoPrint server IP address)
