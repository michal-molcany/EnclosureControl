#define OFFICE 1

class secrets
{
public:
    String getWiFiSSID();
    String getWiFiPassword();
    String getOctoprintApiKey();
};

#if OFFICE
String secrets::getWiFiSSID()
{
    return "ERNI Device Network";
}

String secrets::getWiFiPassword()
{
    return "I@mAr0b0t";
}

String secrets::getOctoprintApiKey()
{
    return "uQRoAg8f6GB0xTTcgJoiR93PnWJEviUEIYOtZTAanKg";
}
#define OCTOTPRINT_IP "10.201.93.207" // IP address of the OctoPrint server (change this to your OctoPrint server IP address)
#define OCTOTPRINT_PORT 80            // Port of the OctoPrint server

#else
String secrets::getWiFiSSID()
{
    return "smajltron";
}

String secrets::getWiFiPassword()
{
    return "kluc je pod rohozkou";
}

String secrets::getOctoprintApiKey()
{
    return "Lr0cAzOdnbCoTW2xJTkeHTZ4s1R9e4D5AEv6KOYEErs";
}
#define OCTOTPRINT_IP "192.168.0.221" // IP address of the OctoPrint server (change this to your OctoPrint server IP address)
#define OCTOTPRINT_PORT 5000          // Port of the OctoPrint server

#endif