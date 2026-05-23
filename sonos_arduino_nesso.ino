#include <Arduino_Nesso_N1.h>
#include <WiFi.h>
#include <esp_http_client.h>

#define ESP_LOGE(dummy, ...) Serial.printf(__VA_ARGS__); Serial.println("")

// replace with your network credentials and sonos setup
const char* ssid = "<ssid>";
const char* password = "<password>";
const char* tvName = "TV Beam";
const std::vector<char*> musicGroupNames = { "Dining", "Kitchen", "Couch" };

NessoDisplay display; // Create a display instance

struct SonosSpeaker
{
    std::string ip;
    std::string uuid;
    std::string room_name;

    std::string av_transport_uri;

    bool is_coordinator = false;

    std::string coordinator_uuid;
};

std::vector<SonosSpeaker> speakers;
std::string tvIP;
std::string tvID;
std::string musicCoordinator;

static int parse_volume(const char *xml)
{
    const char *start = strstr(xml, "<CurrentVolume>");

    if (!start)
    {
        return -1;
    }

    start += strlen("<CurrentVolume>");

    const char *end = strstr(start, "</CurrentVolume>");

    if (!end)
    {
        return -1;
    }

    char volume_str[16] = {};

    int len = end - start;

    if (len >= sizeof(volume_str))
    {
        return -1;
    }

    memcpy(volume_str, start, len);

    return atoi(volume_str);
}

static char response_buffer[4096];
static int response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_DATA:

            if (!esp_http_client_is_chunked_response(evt->client))
            {
                int copy_len = evt->data_len;

                if ((response_len + copy_len) >= sizeof(response_buffer))
                {
                    copy_len =
                        sizeof(response_buffer) -
                        response_len -
                        1;
                }

                memcpy(
                    response_buffer + response_len,
                    evt->data,
                    copy_len);

                response_len += copy_len;

                response_buffer[response_len] = '\0';
            }

            break;

        default:
            break;
    }

    return ESP_OK;
}

static int sonos_get_volume(const char *speaker_ip)
{
    response_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    char url[128];

    snprintf(
        url,
        sizeof(url),
        "http://%s:1400/MediaRenderer/RenderingControl/Control",
        speaker_ip);

    const char *soap_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:GetVolume "
        "xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
        "<InstanceID>0</InstanceID>"
        "<Channel>Master</Channel>"
        "</u:GetVolume>"
        "</s:Body>"
        "</s:Envelope>";

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_header(
        client,
        "Content-Type",
        "text/xml; charset=\"utf-8\"");

    esp_http_client_set_header(
        client,
        "SOAPACTION",
        "\"urn:schemas-upnp-org:service:RenderingControl:1#GetVolume\"");

    esp_http_client_set_post_field(
        client,
        soap_body,
        strlen(soap_body));

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "HTTP request failed: %s",
            esp_err_to_name(err));

        esp_http_client_cleanup(client);

        return -1;
    }

    int status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP status: %d", status);

    esp_http_client_cleanup(client);

    if (status != 200)
    {
        ESP_LOGE(TAG, "Sonos returned non-200");

        return -1;
    }

    return parse_volume(response_buffer);
}

static bool sonos_set_volume(
    const char *speaker_ip,
    int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    char url[128];

    snprintf(
        url,
        sizeof(url),
        "http://%s:1400/MediaRenderer/RenderingControl/Control",
        speaker_ip);

    char soap_body[1024];

    snprintf(
        soap_body,
        sizeof(soap_body),

        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"

        "<s:Body>"

        "<u:SetVolume "
        "xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"

        "<InstanceID>0</InstanceID>"
        "<Channel>Master</Channel>"
        "<DesiredVolume>%d</DesiredVolume>"

        "</u:SetVolume>"

        "</s:Body>"
        "</s:Envelope>",

        volume);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_header(
        client,
        "Content-Type",
        "text/xml; charset=\"utf-8\"");

    esp_http_client_set_header(
        client,
        "SOAPACTION",
        "\"urn:schemas-upnp-org:service:RenderingControl:1#SetVolume\"");

    esp_http_client_set_post_field(
        client,
        soap_body,
        strlen(soap_body));

    esp_err_t err =
        esp_http_client_perform(client);

    int status =
        esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "SetVolume failed: %s",
            esp_err_to_name(err));

        return false;
    }

    if (status != 200)
    {
        ESP_LOGE(
            TAG,
            "SetVolume HTTP status: %d",
            status);

        return false;
    }

    return true;
}

static void discover_sonos_devices()
{
    // ============================================
    // SSDP SEARCH MESSAGE
    // ============================================

    const char *search_request =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
        "\r\n";

    // ============================================
    // CREATE UDP SOCKET
    // ============================================

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock < 0)
    {
        ESP_LOGE(TAG, "Failed to create socket");
        return;
    }

    // ============================================
    // RECEIVE TIMEOUT
    // ============================================

    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;

    setsockopt(
        sock,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout));

    // ============================================
    // MULTICAST DESTINATION
    // ============================================

    struct sockaddr_in dest_addr = {};

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(1900);

    inet_pton(
        AF_INET,
        "239.255.255.250",
        &dest_addr.sin_addr);

    // ============================================
    // SEND M-SEARCH
    // ============================================

    int err = sendto(
        sock,
        search_request,
        strlen(search_request),
        0,
        (struct sockaddr *)&dest_addr,
        sizeof(dest_addr));

    if (err < 0)
    {
        ESP_LOGE(TAG, "Failed to send SSDP search");

        close(sock);

        return;
    }

    ESP_LOGI(TAG, "SSDP discovery sent");

    // ============================================
    // RECEIVE RESPONSES
    // ============================================

    char rx_buffer[2048];

    while (true)
    {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(
            sock,
            rx_buffer,
            sizeof(rx_buffer) - 1,
            0,
            (struct sockaddr *)&source_addr,
            &socklen);

        // timeout or error
        if (len < 0)
        {
            break;
        }

        rx_buffer[len] = 0;

        // ========================================
        // FILTER FOR SONOS
        // ========================================

        //
        // Sonos responses usually contain:
        //
        // SERVER: Linux UPnP/1.0 Sonos
        //
        // or:
        //
        // ST: urn:schemas-upnp-org:device:ZonePlayer:1
        //

        bool is_sonos =
            strstr(rx_buffer, "Sonos") != nullptr ||
            strstr(
                rx_buffer,
                "ZonePlayer:1") != nullptr;

        if (!is_sonos)
        {
            continue;
        }

        // ========================================
        // EXTRACT IP
        // ========================================

        char ip_str[INET_ADDRSTRLEN];

        inet_ntop(
            AF_INET,
            &source_addr.sin_addr,
            ip_str,
            sizeof(ip_str));

        // deduplicate
        bool already_exists = false;

        for (const auto &speaker : speakers)
        {
            if (speaker.ip == ip_str)
            {
                already_exists = true;
                break;
            }
        }

        if (!already_exists)
        {
            SonosSpeaker speaker;
            speaker.ip = ip_str;
            extract_between(rx_buffer, "uuid:", "::urn:", speaker.uuid);
            speakers.push_back(speaker);

            Serial.println(("found device: " + std::string(rx_buffer)).c_str());

            ESP_LOGI(
                TAG,
                "Found Sonos speaker: %s",
                ip_str);

            ESP_LOGI(
                TAG,
                "SSDP response:\n%s",
                rx_buffer);
        }
    }

    close(sock);

    ESP_LOGI(
        TAG,
        "Discovery complete. %d device(s) found",
        (int)devices.size());
}

static char zone_attr_buffer[4096];
static int zone_attr_len = 0;

static esp_err_t zone_attr_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_DATA:
        {
            if (!esp_http_client_is_chunked_response(evt->client))
            {
                int copy_len = evt->data_len;

                if ((zone_attr_len + copy_len) >= sizeof(zone_attr_buffer))
                {
                    copy_len =
                        sizeof(zone_attr_buffer) -
                        zone_attr_len - 1;
                }

                memcpy(
                    zone_attr_buffer + zone_attr_len,
                    evt->data,
                    copy_len);

                zone_attr_len += copy_len;

                zone_attr_buffer[zone_attr_len] = 0;
            }
            break;
        }

        default:
            break;
    }

    return ESP_OK;
}

bool sonos_get_zone_attributes(
    const char *speaker_ip,
    std::string &out_xml)
{
    zone_attr_len = 0;
    memset(zone_attr_buffer, 0, sizeof(zone_attr_buffer));

    char url[128];

    snprintf(
        url,
        sizeof(url),
        "http://%s:1400/DeviceProperties/Control",
        speaker_ip);

    const char *soap_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"

        "<s:Body>"

        "<u:GetZoneAttributes "
        "xmlns:u=\"urn:schemas-upnp-org:service:DeviceProperties:1\">"

        "</u:GetZoneAttributes>"

        "</s:Body>"
        "</s:Envelope>";

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .event_handler = zone_attr_event_handler,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_header(
        client,
        "Content-Type",
        "text/xml; charset=\"utf-8\"");

    esp_http_client_set_header(
        client,
        "SOAPACTION",
        "\"urn:schemas-upnp-org:service:DeviceProperties:1#GetZoneAttributes\"");

    esp_http_client_set_post_field(
        client,
        soap_body,
        strlen(soap_body));

    esp_err_t err =
        esp_http_client_perform(client);

    int status =
        esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "sonos_get_zone_attributes() failed: %s",
            esp_err_to_name(err));

        return false;
    }

    if (status != 200)
    {
        ESP_LOGE(
            TAG,
            "sonos_get_zone_attributes() HTTP status: %d",
            status);

        return false;
    }

    out_xml = zone_attr_buffer;

    return true;
}

void identify_sonos_speakers()
{
    for (auto &speaker : speakers)
    {
        // ---------------------------------------
        // 1. Get room name
        // (from device description XML or GetZoneAttributes)
        // ---------------------------------------

        std::string zone_xml;

        while (true)
        {
            if (!sonos_get_zone_attributes(speaker.ip.c_str(), zone_xml))
            {
                Serial.println("failed to get zone attributes, retrying...");
                delay(50);
                continue;
            }
            if (!extract_between(zone_xml, "<CurrentZoneName>", "</CurrentZoneName>", speaker.room_name))
            {
                Serial.println(zone_xml.c_str());
                Serial.println("failed to extract room name, retrying...");
                delay(50);
                continue;
            }
            break;
        }

        // ---------------------------------------
        // 2. Get AVTransportURI (group info)
        // ---------------------------------------

        std::string media_xml;

        while (true)
        {
            if (!sonos_get_media_info(speaker.ip.c_str(), media_xml))
            {
                Serial.println("failed to get media info, retrying...");
                delay(50);
                continue;
            }
            if (!extract_between(media_xml, "<CurrentURI>", "</CurrentURI>", speaker.av_transport_uri))
            {
                Serial.printf("failed to extract URI for %s, retrying...\n", speaker.room_name.c_str());
                delay(50);
                //continue;
            }
            break;
        }
    }
}

bool sonos_join_group(
    const char *speaker_ip,
    const char *coordinator_uuid)
{
    char url[128];

    snprintf(
        url,
        sizeof(url),
        "http://%s:1400/MediaRenderer/AVTransport/Control",
        speaker_ip);

    char soap_body[2048];

    snprintf(
        soap_body,
        sizeof(soap_body),

        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"

        "<s:Body>"

        "<u:SetAVTransportURI "
        "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"

        "<InstanceID>0</InstanceID>"

        "<CurrentURI>"
        "x-rincon:%s"
        "</CurrentURI>"

        "<CurrentURIMetaData></CurrentURIMetaData>"

        "</u:SetAVTransportURI>"

        "</s:Body>"
        "</s:Envelope>",

        coordinator_uuid);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_header(
        client,
        "Content-Type",
        "text/xml; charset=\"utf-8\"");

    esp_http_client_set_header(
        client,
        "SOAPACTION",
        "\"urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI\"");

    esp_http_client_set_post_field(
        client,
        soap_body,
        strlen(soap_body));

    esp_err_t err =
        esp_http_client_perform(client);

    int status =
        esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    return (err == ESP_OK && status == 200);
}

static bool sonos_get_media_info(
    const char *speaker_ip,
    std::string &response)
{
    response.clear();

    char url[128];

    snprintf(
        url,
        sizeof(url),
        "http://%s:1400/MediaRenderer/AVTransport/Control",
        speaker_ip);

    const char *soap_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"

        "<s:Body>"

        "<u:GetMediaInfo "
        "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"

        "<InstanceID>0</InstanceID>"

        "</u:GetMediaInfo>"

        "</s:Body>"
        "</s:Envelope>";

    static char response_buffer[8192];
    static int response_len = 0;

    response_len = 0;

    auto event_handler =
        [](esp_http_client_event_t *evt) -> esp_err_t
    {
        if (evt->event_id == HTTP_EVENT_ON_DATA)
        {
            int copy_len = evt->data_len;

            if ((response_len + copy_len) >=
                sizeof(response_buffer))
            {
                copy_len =
                    sizeof(response_buffer) -
                    response_len -
                    1;
            }

            memcpy(
                response_buffer + response_len,
                evt->data,
                copy_len);

            response_len += copy_len;

            response_buffer[response_len] = 0;
        }

        return ESP_OK;
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .event_handler = event_handler,
    };

    auto client =
        esp_http_client_init(&config);

    esp_http_client_set_header(
        client,
        "Content-Type",
        "text/xml; charset=\"utf-8\"");

    esp_http_client_set_header(
        client,
        "SOAPACTION",
        "\"urn:schemas-upnp-org:service:AVTransport:1#GetMediaInfo\"");

    esp_http_client_set_post_field(
        client,
        soap_body,
        strlen(soap_body));

    esp_err_t err =
        esp_http_client_perform(client);

    int status =
        esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "sonos_get_media_info() failed: %s",
            esp_err_to_name(err));

        return false;
    }

    if (status != 200)
    {
        ESP_LOGE(
            TAG,
            "sonos_get_media_info() HTTP status: %d",
            status);

        return false;
    }

    response = response_buffer;

    return true;
}

static bool extract_between(
    const std::string &body,
    const std::string start_tag,
    const std::string end_tag,
    std::string &out)
{
    size_t start =
        body.find(start_tag);

    if (start == std::string::npos)
    {
        return false;
    }

    start += start_tag.length();

    size_t end =
        body.find(end_tag, start);

    if (end == std::string::npos)
    {
        return false;
    }

    out = body.substr(start, end - start);

    return true;
}

static void analyze_topology(
    std::vector<SonosSpeaker>& speakers);

static void analyze_topology(
    std::vector<SonosSpeaker>& speakers)
{
    for (auto &speaker : speakers)
    {
        //
        // follower
        //
        // x-rincon:RINCON_COORDINATOR
        //

        if (speaker.av_transport_uri.find("x-rincon:") == 0)
        {
            speaker.is_coordinator = false;

            speaker.coordinator_uuid =
                speaker.av_transport_uri.substr(
                    strlen("x-rincon:"));
        }
        else
        {
            //
            // owns own queue
            // therefore coordinator/standalone
            //

            speaker.is_coordinator = true;

            speaker.coordinator_uuid =
                speaker.uuid;
        }
    }
}

static bool sonos_switch_to_tv(
    const char *speaker_ip,
    const char *speaker_uuid)
{
    char url[128];

    snprintf(
        url,
        sizeof(url),
        "http://%s:1400/MediaRenderer/AVTransport/Control",
        speaker_ip);

    char soap_body[2048];

    snprintf(
        soap_body,
        sizeof(soap_body),

        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"

        "<s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"

        "<s:Body>"

        "<u:SetAVTransportURI "
        "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"

        "<InstanceID>0</InstanceID>"

        "<CurrentURI>"
        "x-sonos-htastream:%s:spdif"
        "</CurrentURI>"

        "<CurrentURIMetaData></CurrentURIMetaData>"

        "</u:SetAVTransportURI>"

        "</s:Body>"
        "</s:Envelope>",

        speaker_uuid);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_header(
        client,
        "Content-Type",
        "text/xml; charset=\"utf-8\"");

    esp_http_client_set_header(
        client,
        "SOAPACTION",
        "\"urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI\"");

    esp_http_client_set_post_field(
        client,
        soap_body,
        strlen(soap_body));

    esp_err_t err =
        esp_http_client_perform(client);

    int status =
        esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "TV switch failed: %s",
            esp_err_to_name(err));

        return false;
    }

    if (status != 200)
    {
        ESP_LOGE(
            TAG,
            "TV switch HTTP status: %d",
            status);

        return false;
    }

    return true;
}

void print_speaker_info()
{
  for (const SonosSpeaker& speaker : speakers)
  {
    Serial.println((speaker.room_name + (speaker.is_coordinator ? " *" : "") + ":").c_str());
    Serial.println(("  IP: " + speaker.ip).c_str());
    Serial.println(("  UUID: " + speaker.uuid).c_str());
    Serial.println(("  URI: " + speaker.av_transport_uri).c_str());
    Serial.println(("  coordinator: " + speaker.coordinator_uuid).c_str());
  }
}

struct DisplayState
{
    int volume{ -1 };
    bool tvMode{ true };
    int page{ 0 };
};

DisplayState lastDisplayState;
DisplayState currentDisplayState;
int frameCount = 0;

void setup() {
  display.begin();
  display.setRotation(3); // Set to landscape mode

  // Set text properties
  display.setTextDatum(MC_DATUM); // Middle-Center datum for text alignment

  // Clear the screen and draw the string
  display.fillScreen(TFT_BLACK);
  display.setFont(&fonts::Font4);
  display.setTextSize(1);
  display.drawString("connecting to wifi...", display.width() / 2, display.height() / 2);

  Serial.begin(115200);
  delay(1000); // Give serial a moment to initialize

  Serial.println("Connecting to Wi-Fi...");
  
  // Start Wi-Fi connection
  WiFi.begin(ssid, password);

  // Wait until the connection is established
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  int counter = 0;
  do
  {
    display.fillScreen(TFT_BLACK);
    if (counter % 3 == 0)
        display.drawString("discovering sonos...", display.width() / 2, display.height() / 2);
    if (counter % 3 == 1)
        display.drawString("discovering sonos.  ", display.width() / 2, display.height() / 2);
    if (counter % 3 == 2)
        display.drawString("discovering sonos.. ", display.width() / 2, display.height() / 2);
    discover_sonos_devices();
    ++counter;
  } while (speakers.size() == 0);

  display.fillScreen(TFT_BLACK);
  display.drawString("identifying sonos.. ", display.width() / 2, display.height() / 2);
  identify_sonos_speakers();

  display.fillScreen(TFT_BLACK);
  display.drawString("analyzing sonos.. ", display.width() / 2, display.height() / 2);
  analyze_topology(speakers);

  for (const SonosSpeaker& speaker : speakers)
  {
    if (speaker.room_name == tvName)
    {
       tvIP = speaker.ip;
       tvID = speaker.uuid;
    }
    if (std::find(musicGroupNames.begin(), musicGroupNames.end(), speaker.room_name) != musicGroupNames.end())
       musicCoordinator = speaker.coordinator_uuid;
  }

  print_speaker_info();
}

void loop() {
  delay(100);

  if (digitalRead(KEY1) == LOW)
  {
    currentDisplayState.tvMode = !currentDisplayState.tvMode;
    if (currentDisplayState.tvMode)
    {
        Serial.println("switching to TV mode");
        sonos_set_volume(tvIP.c_str(), 70);
        currentDisplayState.volume = 70;
        sonos_join_group(tvIP.c_str(), tvID.c_str());
        //delay(500);
        sonos_switch_to_tv(tvIP.c_str(), tvID.c_str());
    }
    else
    {
        Serial.println("switching to music mode");
        sonos_set_volume(tvIP.c_str(), 30);
        currentDisplayState.volume = 30;
        sonos_join_group(tvIP.c_str(), musicCoordinator.c_str());
    }

    do
    {
        delay(50);
    } while (digitalRead(KEY1) == LOW);
  }

  if (digitalRead(KEY2) == LOW)
  {
    currentDisplayState.page = (currentDisplayState.page + 1) % 2;

    if (currentDisplayState.page == 1)
       print_speaker_info();

    do
    {
        delay(50);
    } while (digitalRead(KEY2) == LOW);
  }

  ++frameCount;
  if (frameCount >= 10)
  {
    currentDisplayState.volume = sonos_get_volume(tvIP.c_str());
    frameCount = 0;
  }

  if (memcmp(&lastDisplayState, &currentDisplayState, sizeof(DisplayState)) == 0)
    return;

  memcpy(&lastDisplayState, &currentDisplayState, sizeof(DisplayState));

  display.fillScreen(TFT_BLACK);

  if (currentDisplayState.page == 0)
  {
    display.setTextDatum(MC_DATUM); // Middle-Center datum for text alignment

    char volumeStr[5];
    itoa(currentDisplayState.volume, volumeStr, 10);
    display.setFont(&fonts::Font8);
    display.setTextSize(1.4f);
    display.setTextColor(TFT_DARKGRAY, TFT_BLACK);
    display.drawString(volumeStr, 75, display.height() / 2);

    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.setTextColor(currentDisplayState.tvMode ? TFT_BLACK : TFT_DARKGRAY, currentDisplayState.tvMode ? TFT_DARKGRAY : TFT_BLACK);
    display.drawString("  TV  ", display.width() - 40, display.height() / 4);
    display.setTextColor(currentDisplayState.tvMode ? TFT_DARKGRAY : TFT_BLACK, currentDisplayState.tvMode ? TFT_BLACK : TFT_DARKGRAY);
    display.drawString("music", display.width() - 40, display.height() / 4 * 3);
  }

  if (currentDisplayState.page == 1)
  {
    display.setTextDatum(TL_DATUM); // top-left datum for text alignment
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    display.setFont(&fonts::Font4);
    display.setTextSize(.5f);
    int x = 4;
    int y = 4;
    display.drawString((std::to_string(speakers.size()) + " speakers: ").c_str(), x, y);
    for (const SonosSpeaker& speaker : speakers)
    {
        y += 10;
        display.drawString((speaker.room_name + (speaker.is_coordinator ? "*" : "")).c_str(), x, y);
    }
  }
}