#pragma once

#include <cJSON.h>
#include <string>

class MossConfigService {
public:
    static MossConfigService& GetInstance();

    void OnNetworkConnected();
    void EnterBindMode();
    void LeaveBindMode();
    void CacheIdentity();
    bool IsBindMode() const { return bind_mode_; }

    std::string Hostname() const;
    std::string InstanceName() const;
    const std::string& CachedMac() const { return cached_mac_; }
    const std::string& CachedClientId() const { return cached_client_id_; }
    const std::string& CachedDisplayName() const { return cached_display_name_; }
    const std::string& CachedVersion() const { return cached_version_; }

    void StartLanServices();

private:
    MossConfigService();
    ~MossConfigService();

    void StartMdns();
    void StopMdns();

    bool bind_mode_ = false;
    bool mdns_started_ = false;
    std::string hostname_;
    std::string cached_mac_;
    std::string cached_client_id_;
    std::string cached_display_name_;
    std::string cached_version_;
};
