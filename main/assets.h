#ifndef ASSETS_H
#define ASSETS_H

#include <string>
#include "asset_mapping.h"
#include <functional>
#include <memory>
#include <cstdint>

#include <cJSON.h>
#include <esp_partition.h>
#include <model_path.h>
#include <map>
#include <string>

#if HAVE_LVGL
#include <spi_flash_mmap.h>
#endif

struct Asset {
    size_t size;
    size_t offset;
};

class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets();

    bool Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback);
    bool Apply();
    bool LoadAssistantModels(srmodel_list_t*& models);
    bool BeginTrialHealth(bool models_ready, int64_t now_ms, uint32_t wake_feeds);
    bool PollTrialHealth(int64_t now_ms, bool healthy, uint32_t wake_feeds);
    bool UpdatesReady() const;
    std::string CurrentVersion() const;
    std::unique_ptr<asset_storage::UpdateSession> BeginUpdate();
    bool GetAssetData(const std::string& name, void*& ptr, size_t& size);

    inline bool partition_valid() const { return partition_valid_; }
    inline std::string default_assets_url() const { return default_assets_url_; }

private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    bool InitializePartition();
    void UnApplyPartition();
    static bool FindPartition(Assets* assets);
    static bool LoadSrmodelsFromIndex(Assets* assets, cJSON* root = nullptr, bool publish = true);
  
    class AssetStrategy {
    public:
        virtual ~AssetStrategy() = default;
        virtual bool Apply(Assets* assets) = 0;
        virtual bool InitializePartition(Assets* assets) = 0;
        virtual void UnApplyPartition(Assets* assets) = 0;
        virtual bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) = 0;
        virtual void PinModels() {}
        virtual bool TrialPending() const { return false; }
        virtual bool BeginTrialHealth(bool, int64_t, uint32_t) { return false; }
        virtual bool PollTrialHealth(int64_t, bool, uint32_t) { return false; }
        virtual bool UpdatesReady() const { return false; }
        virtual std::string CurrentVersion() const { return {}; }
        virtual std::unique_ptr<asset_storage::UpdateSession> BeginUpdate() { return {}; }
    };
    
    class LvglStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) override;
        void PinModels() override;
        bool TrialPending() const override;
        bool BeginTrialHealth(bool models_ready, int64_t now_ms, uint32_t wake_feeds) override;
        bool PollTrialHealth(int64_t now_ms, bool healthy, uint32_t wake_feeds) override;
        bool UpdatesReady() const override;
        std::string CurrentVersion() const override;
        std::unique_ptr<asset_storage::UpdateSession> BeginUpdate() override;
    private:
        std::map<std::string, Asset> assets_;
#if HAVE_LVGL
        std::unique_ptr<asset_storage::Mapping> mapping_;
#endif
        const char* mmap_root_ = nullptr;
        bool checksum_valid_ = false;
    };
    
    class EmoteStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) override;
    };
    
    // Strategy instance
    std::unique_ptr<AssetStrategy> strategy_;

protected:
    const esp_partition_t* partition_ = nullptr;
    bool partition_valid_ = false;
    std::string default_assets_url_;
    srmodel_list_t* models_list_ = nullptr;
    bool models_pinned_ = false;
};

#endif
