#include "platform/network/network_route_source.hpp"

#include <cstdio>
#include <cstring>

#include "esp_netif.h"

namespace micropixel::platform::network {
void NetworkRouteSource::Read(device::NetworkRoute& destination) const {
    destination = {};
    // Route lookup, reads and netif destruction share the TCP/IP context.
    (void)esp_netif_tcpip_exec(
        [](void* context) -> esp_err_t {
            auto& out = *static_cast<device::NetworkRoute*>(context);
            auto* netif = esp_netif_get_default_netif();
            esp_netif_ip_info_t ip{};
            if (!netif || !esp_netif_is_netif_up(netif) || esp_netif_get_ip_info(netif, &ip) != ESP_OK ||
                ip.ip.addr == 0)
                return ESP_ERR_NOT_FOUND;
            const char* key = esp_netif_get_ifkey(netif);
            if (key && std::strcmp(key, "WIFI_STA_DEF") == 0) out.transport = device::NetworkTransport::kWifi;
            // The NT26 driver is the sole Ethernet netif on supported cellular boards.
            else if (key && std::strcmp(key, "ETH_DEF") == 0)
                out.transport = device::NetworkTransport::kCellular;
            else
                return ESP_ERR_NOT_SUPPORTED;
            std::snprintf(out.address.data(), out.address.size(), IPSTR, IP2STR(&ip.ip));
            std::snprintf(out.gateway.data(), out.gateway.size(), IPSTR, IP2STR(&ip.gw));
            std::snprintf(out.netmask.data(), out.netmask.size(), IPSTR, IP2STR(&ip.netmask));
            esp_netif_dns_info_t dns{};
            if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
                std::snprintf(out.dns.data(), out.dns.size(), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
            const char* hostname = nullptr;
            if (esp_netif_get_hostname(netif, &hostname) == ESP_OK && hostname)
                std::snprintf(out.hostname.data(), out.hostname.size(), "%s", hostname);
            uint8_t mac[6]{};
            if (esp_netif_get_mac(netif, mac) == ESP_OK)
                std::snprintf(out.mac.data(), out.mac.size(), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
                              mac[3], mac[4], mac[5]);
            return ESP_OK;
        },
        &destination);
}
}  // namespace micropixel::platform::network
