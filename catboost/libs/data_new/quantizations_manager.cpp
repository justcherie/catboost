#include "quantizations_manager.h"

#include <catboost/libs/quantization/utils.h>

#include <util/generic/mapfindptr.h>


namespace NCB {
    ENanMode TQuantizedFeaturesManager::ComputeNanMode(const TFloatValuesHolder& feature) const {
        if (FloatFeaturesBinarization.NanMode == ENanMode::Forbidden) {
            return ENanMode::Forbidden;
        }
        const TMaybeOwningArraySubset<float>& arrayData = feature.GetArrayData();

        bool hasNans = arrayData.Find([] (size_t /*idx*/, float value) { return IsNan(value); });
        if (hasNans) {
            return FloatFeaturesBinarization.NanMode;
        }
        return ENanMode::Forbidden;
    }

    TQuantizedFeaturesManager& TQuantizedFeaturesManager::SetBinarization(ui64 featureId, TVector<float>&& borders) {
        Borders[featureId] = std::move(borders);
        return *this;
    }

    bool TQuantizedFeaturesManager::IsKnown(const ui32 featuresProviderId) const {
        return DataProviderFloatFeatureIdToFeatureManagerId.has(featuresProviderId) ||
               DataProviderCatFeatureIdToFeatureManagerId.has(featuresProviderId);
    }

    bool TQuantizedFeaturesManager::IsKnown(const IFeatureValuesHolder& feature) const {
        return IsKnown(feature.GetId());
    }

    void TQuantizedFeaturesManager::SetOrCheckNanMode(const IFeatureValuesHolder& feature, ENanMode nanMode)  {
        if (!NanModes.has(feature.GetId())) {
            NanModes[feature.GetId()] = nanMode;
        } else {
            CB_ENSURE(NanModes.at(feature.GetId()) == nanMode, "NaN mode should be consistent " << nanMode);
        }
    }

    ENanMode TQuantizedFeaturesManager::GetOrComputeNanMode(const TFloatValuesHolder& feature)  {
        if (!NanModes.has(feature.GetId())) {
            NanModes[feature.GetId()] = ComputeNanMode(feature);
        }
        return NanModes.at(feature.GetId());
    }

    ENanMode TQuantizedFeaturesManager::GetNanMode(const ui32 featureId) const  {
        ENanMode nanMode = ENanMode::Forbidden;
        if (FeatureManagerIdToDataProviderId.has(featureId)) {
            CB_ENSURE(IsFloat(featureId));
            const ui32 dataProviderId = FeatureManagerIdToDataProviderId[featureId];
            if (NanModes.has(dataProviderId)) {
                nanMode = NanModes.at(dataProviderId);
            }
        }
        return nanMode;
    }

    const TVector<float>& TQuantizedFeaturesManager::GetFloatFeatureBorders(const TFloatValuesHolder& feature) const {
        CB_ENSURE(IsKnown(feature));
        ui32 id = GetId(feature);
        return Borders.at(id);
    }

    bool TQuantizedFeaturesManager::IsFloat(ui32 featureId) const  {
        if (FeatureManagerIdToDataProviderId.has(featureId)) {
            return DataProviderFloatFeatureIdToFeatureManagerId.has(FeatureManagerIdToDataProviderId.at(featureId));
        } else {
            return false;
        }
    }

    bool TQuantizedFeaturesManager::IsCat(ui32 featureId) const {
        if (FeatureManagerIdToDataProviderId.has(featureId)) {
            return DataProviderCatFeatureIdToFeatureManagerId.has(FeatureManagerIdToDataProviderId.at(featureId));
        } else {
            return false;
        }
    }

    TVector<ui32> TQuantizedFeaturesManager::GetOneHotIds(const TVector<ui32>& ids) const {
        TVector<ui32> result;
        result.reserve(ids.size());
        for (auto id : ids) {
            if (IsCat(id) && UseForOneHotEncoding(id)) {
                result.push_back(id);
            }
        }
        return result;
    }

    bool TQuantizedFeaturesManager::UseForOneHotEncoding(ui32 featureId) const  {
        CB_ENSURE(IsCat(featureId));
        const ui32 uniqueValues = GetUniqueValues(featureId);
        return (uniqueValues > 1) && uniqueValues <= CatFeatureOptions.OneHotMaxSize;
    }

    bool TQuantizedFeaturesManager::UseForCtr(ui32 featureId) const {
        CB_ENSURE(IsCat(featureId));
        return GetUniqueValues(featureId) > Max<ui32>(1u, CatFeatureOptions.OneHotMaxSize);
    }

    bool TQuantizedFeaturesManager::UseForTreeCtr(ui32 featureId) const  {
        CB_ENSURE(IsCat(featureId));
        return GetUniqueValues(featureId) > Max<ui32>(1u, CatFeatureOptions.OneHotMaxSize) &&
               (CatFeatureOptions.MaxTensorComplexity > 1);
    }

    bool TQuantizedFeaturesManager::IsPermutationDependent(const TCtr& ctr) const {
        if (IsPermutationDependentCtrType(ctr.Configuration.Type)) {
            return true;
        }
        return HasPermutationDependentSplit(ctr.FeatureTensor.GetSplits());
    }

    bool TQuantizedFeaturesManager::HasPermutationDependentSplit(const TVector<TBinarySplit>& splits) const  {
        for (auto& split : splits) {
            if (IsCtr(split.FeatureId) && IsPermutationDependent(GetCtr(split.FeatureId))) {
                return true;
            }
        }
        return false;
    }

    ui32 TQuantizedFeaturesManager::GetFeatureManagerId(const IFeatureValuesHolder& feature) const {
        switch (feature.GetType()) {
            case EFeatureValuesType::QuantizedFloat:
            case EFeatureValuesType::Float: {
                return GetFeatureManagerIdForFloatFeature(feature.GetId());
            }
            case EFeatureValuesType::HashedCategorical:
            case EFeatureValuesType::PerfectHashedCategorical: {
                return GetFeatureManagerIdForCatFeature(feature.GetId());
            }
            default: {
                ythrow TCatboostException() << "Unknown feature id " << feature.GetId();
            }
        }
    }

    ui32 TQuantizedFeaturesManager::GetFeatureManagerIdForCatFeature(ui32 dataProviderId) const {
        CB_ENSURE(DataProviderCatFeatureIdToFeatureManagerId.has(dataProviderId),
                  "Error: feature #" << dataProviderId << " is not categorical");
        return DataProviderCatFeatureIdToFeatureManagerId.at(dataProviderId);
    }

    ui32 TQuantizedFeaturesManager::GetFeatureManagerIdForFloatFeature(ui32 dataProviderId) const  {
        CB_ENSURE(DataProviderFloatFeatureIdToFeatureManagerId.has(dataProviderId),
                  "Error: feature #" << dataProviderId << " is not float");
        return DataProviderFloatFeatureIdToFeatureManagerId.at(dataProviderId);
    }

    ui32 TQuantizedFeaturesManager::CtrsPerTreeCtrFeatureTensor() const  {
        ui32 totalCount = 0;
        auto treeCtrConfigs = CreateGrouppedTreeCtrConfigs();
        for (const auto& treeCtr : treeCtrConfigs) {
            totalCount += treeCtr.second.size();
        }
        return totalCount;
    }

    ui32 TQuantizedFeaturesManager::GetBinCount(ui32 localId) const {
        if (auto bordersPtr = MapFindPtr(Borders, localId)) {
            return NCB::GetBinCount(*bordersPtr, GetNanMode(localId));
        } else if (IsCat(localId)) {
            return GetUniqueValues(localId);
        } else if (auto inverseCtrsPtr = MapFindPtr(InverseCtrs, localId)) {
            return GetBinarizationDescription(*inverseCtrsPtr).BorderCount + 1;
        } else if (IsFloat(localId)) {
            return 0;
        } else {
            ythrow TCatboostException() << "Error: unknown feature id #" << localId;
        }
    }

    ui32 TQuantizedFeaturesManager::AddCtr(const TCtr& ctr)  {
        CB_ENSURE(!KnownCtrs.has(ctr));
        const ui32 id = RequestNewId();
        KnownCtrs[ctr] = id;
        InverseCtrs[id] = ctr;
        return id;
    }

    TSet<ECtrType> TQuantizedFeaturesManager::GetKnownSimpleCtrTypes() const  {
        TSet<ECtrType> result;
        auto simpleCtrs = CreateGrouppedSimpleCtrConfigs();
        for (const auto& simpleCtr : simpleCtrs) {
            result.insert(simpleCtr.first);
        }
        auto perFeatureCtrs = CreateGrouppedPerFeatureCtrs();
        for (const auto& perFeatureCtr : perFeatureCtrs) {
            for (const auto& grouppedCtr : perFeatureCtr.second) {
                result.insert(grouppedCtr.first);
            }
        }
        return result;
    }

    TVector<ui32> TQuantizedFeaturesManager::CreateSimpleCtrsForType(ui32 featureId, ECtrType type)  {
        CB_ENSURE(UseForCtr(featureId));
        TSet<ui32> resultIds;

        if (HasPerFeatureCtr(featureId)) {
            auto perFeatureCtrs = CreateGrouppedPerFeatureCtr(featureId);
            if (perFeatureCtrs.has(type)) {
                CreateSimpleCtrs(featureId, perFeatureCtrs.at(type), &resultIds);
            }
        } else {
            auto simpleCtrConfigs = CreateGrouppedSimpleCtrConfigs();
            CB_ENSURE(simpleCtrConfigs.at(type), "Simple ctr type is not enabled " << type);
            CreateSimpleCtrs(featureId, simpleCtrConfigs.at(type), &resultIds);
        }

        return TVector<ui32>(resultIds.begin(), resultIds.end());
    }

    TVector<ui32> TQuantizedFeaturesManager::GetAllSimpleCtrs() const  {
        TVector<ui32> ids;
        ids.reserve(KnownCtrs.size());
        for (auto& knownCtr : KnownCtrs) {
            const TCtr& ctr = knownCtr.first;
            if (ctr.IsSimple()) {
                ids.push_back(knownCtr.second);
            }
        }
        return ids;
    }

    TVector<ui32> TQuantizedFeaturesManager::CreateCombinationCtrForType(ECtrType type) {
        TSet<ui32> resultIds;

        for (auto& combination : UserCombinations) {
            if (combination.Description.Type != type) {
                continue;
            }
            TMap<ECtrType, TSet<TCtrConfig>> configs;
            CreateCtrConfigsFromDescription(combination.Description, &configs);
            if (configs.has(type)) {
                for (auto& ctrConfig : configs[type]) {
                    TCtr ctr;
                    ctr.FeatureTensor = combination.Tensor;
                    ctr.Configuration = ctrConfig;

                    if (!KnownCtrs.has(ctr)) {
                        AddCtr(ctr);
                    }
                    resultIds.insert(GetId(ctr));
                }
            }
        }
        return TVector<ui32>(resultIds.begin(), resultIds.end());
    }

    void TQuantizedFeaturesManager::CreateSimpleCtrs(const ui32 featureId, const TSet<TCtrConfig>& configs,
                                                     TSet<ui32>* resultIds)  {
        for (const auto& ctrConfig : configs) {
            TCtr ctr;
            ctr.FeatureTensor.AddCatFeature(featureId);
            ctr.Configuration = ctrConfig;

            if (!KnownCtrs.has(ctr)) {
                AddCtr(ctr);
            }
            resultIds->insert(GetId(ctr));
        }
    }

    ui32 TQuantizedFeaturesManager::GetId(const IFeatureValuesHolder& feature) const {
        const ui32 featureId = feature.GetId();

        if (DataProviderFloatFeatureIdToFeatureManagerId.has(featureId)) {
            return DataProviderFloatFeatureIdToFeatureManagerId[featureId];
        } else if (DataProviderCatFeatureIdToFeatureManagerId.has(featureId)) {
            return DataProviderCatFeatureIdToFeatureManagerId[featureId];
        } else {
            ythrow TCatboostException() << "Error: unknown feature with id #" << feature.GetId();
        }
    }

    TVector<NCB::TCtrConfig> TQuantizedFeaturesManager::CreateTreeCtrConfigs(ETaskType taskType) const {
        TVector<NCB::TCtrConfig> result;
        auto treeCtrConfigs = CreateGrouppedTreeCtrConfigs();

        for (const auto& ctrConfigs : treeCtrConfigs) {
            auto ctrType = ctrConfigs.first;
            CB_ENSURE(IsSupportedCtrType(taskType, ctrType));
            for (const auto& ctrConfig : ctrConfigs.second) {
                result.push_back(ctrConfig);
            }
        }
        return result;
    }

    TMap<ECtrType, TSet<TCtrConfig>> TQuantizedFeaturesManager::CreateGrouppedTreeCtrConfigs() const {
        TMap<ECtrType, TSet<TCtrConfig>> treeCtrConfigs;
        for (const auto& treeCtr : CatFeatureOptions.CombinationCtrs.Get()) {
            CreateCtrConfigsFromDescription(treeCtr, &treeCtrConfigs);
        }
        return treeCtrConfigs;
    }

    ui32 TQuantizedFeaturesManager::MaxTreeCtrBinFeaturesCount() const {
        ui32 total = 0;
        auto treeCtrConfigs = CreateGrouppedTreeCtrConfigs();
        for (const auto& treeCtrConfig : treeCtrConfigs) {
            for (auto& config : treeCtrConfig.second) {
                ui32 binarization = GetCtrBinarizationForConfig(config).BorderCount + 1;
                total += binarization;
            }
        }
        return total;
    }

    TVector<ui32> TQuantizedFeaturesManager::GetCatFeatureIds() const {
        TVector<ui32> featureIds;
        featureIds.reserve(DataProviderCatFeatureIdToFeatureManagerId.size());

        for (const auto& feature : DataProviderCatFeatureIdToFeatureManagerId) {
            if (GetBinCount(feature.second)) {
                featureIds.push_back(feature.second);
            }
        }
        Sort(featureIds.begin(), featureIds.end());
        return featureIds;
    }

    TVector<ui32> TQuantizedFeaturesManager::GetFloatFeatureIds() const {
        TVector<ui32> featureIds;
        featureIds.reserve(DataProviderFloatFeatureIdToFeatureManagerId.size());

        for (const auto& feature : DataProviderFloatFeatureIdToFeatureManagerId) {
            if (GetBinCount(feature.second) > 1) {
                featureIds.push_back(feature.second);
            }
        }
        return featureIds;
    }

    TVector<ui32> TQuantizedFeaturesManager::GetDataProviderFeatureIds() const  {
        TVector<ui32> features;
        {
            auto floatFeatureIds = GetFloatFeatureIds();
            for (auto id : floatFeatureIds) {
                Y_ASSERT(GetBinCount(id));
            }
            features = std::move(floatFeatureIds);
        }
        {
            auto catFeatureIds = GetCatFeatureIds();
            for (auto id : catFeatureIds) {
                Y_ASSERT(GetBinCount(id));
            }
            features.insert(features.end(), catFeatureIds.begin(), catFeatureIds.end());
        }
        return features;
    }

    TMap<ECtrType, TSet<TCtrConfig>> TQuantizedFeaturesManager::CreateGrouppedSimpleCtrConfigs() const {
        TMap<ECtrType, TSet<TCtrConfig>> simpleCtrs;

        for (const auto& simpleCtr : CatFeatureOptions.SimpleCtrs.Get()) {
            CreateCtrConfigsFromDescription(simpleCtr, &simpleCtrs);
        }
        return simpleCtrs;
    }

    TMap<ui32, TMap<ECtrType, TSet<TCtrConfig>>> TQuantizedFeaturesManager::CreateGrouppedPerFeatureCtrs() const  {
        TMap<ui32, TMap<ECtrType, TSet<TCtrConfig>>> perFeatureCtrs;

        for (const auto& perFeatureCtr : CatFeatureOptions.PerFeatureCtrs.Get()) {
            CB_ENSURE(DataProviderCatFeatureIdToFeatureManagerId.has(perFeatureCtr.first),
                      "Error: Feature with id #" << perFeatureCtr.first << " is not categorical. Can't create ctr");
            const ui32 featureId = DataProviderCatFeatureIdToFeatureManagerId[perFeatureCtr.first];
            for (const auto& ctrDescription : perFeatureCtr.second) {
                CreateCtrConfigsFromDescription(ctrDescription, &perFeatureCtrs[featureId]);
            }
        }
        return perFeatureCtrs;
    }

    TMap<ECtrType, TSet<TCtrConfig>> TQuantizedFeaturesManager::CreateGrouppedPerFeatureCtr(ui32 featureId) const  {
        CB_ENSURE(IsCat(featureId), "Feature #" << featureId << " is not categorical. Can't create per feature CTRs");
        ui32 featureIdInPool = GetDataProviderId(featureId);
        CB_ENSURE(CatFeatureOptions.PerFeatureCtrs->has(featureIdInPool), "No perFeatureCtr for feature #" << featureIdInPool << " was found");
        TMap<ECtrType, TSet<TCtrConfig>> perFeatureCtr;
        for (const auto& ctrDescription : CatFeatureOptions.PerFeatureCtrs->at(featureIdInPool)) {
            CreateCtrConfigsFromDescription(ctrDescription, &perFeatureCtr);
        }
        return perFeatureCtr;
    }

    void TQuantizedFeaturesManager::CreateCtrConfigsFromDescription(const NCatboostOptions::TCtrDescription& ctrDescription,
                                                                    TMap<ECtrType, TSet<TCtrConfig>>* grouppedConfigs) const{
        for (const auto& prior : ctrDescription.GetPriors()) {
            CB_ENSURE(!TargetBorders.empty(), "Enable ctr description should be done after target borders are set");
            CB_ENSURE(ctrDescription.GetPriors().size(), "Set priors first");

            ECtrType type = ctrDescription.Type;
            TCtrConfig defaultConfig;

            defaultConfig.Prior = prior;
            if (defaultConfig.Prior.size() == 1) {
                defaultConfig.Prior.push_back(1);
            }
            defaultConfig.Type = type;
            defaultConfig.CtrBinarizationConfigId = GetOrCreateCtrBinarizationId(ctrDescription.CtrBinarization);
            CB_ENSURE(defaultConfig.Prior.size() == 2, "Error: currently priors are num and denum biases. Need 2 params in option, got " << prior.size());

            if (type == ECtrType::Buckets || type == ECtrType::Borders) {
                ui32 numBins = (type == ECtrType::Buckets)
                               ? TargetBorders.size() + 1
                               : TargetBorders.size();
                const ui32 numCtrs = numBins;

                for (ui32 i = 0; i < numCtrs; ++i) {
                    //don't calc 0-class ctr for binary classification, it's unneeded
                    if (i == 0 && numBins == 2 && type == ECtrType::Buckets) {
                        continue;
                    }
                    TCtrConfig config = defaultConfig;
                    config.ParamId = i;
                    (*grouppedConfigs)[type].insert(config);
                }
            } else {
                (*grouppedConfigs)[type].insert(defaultConfig);
            }
        }
    }

    ui32 TQuantizedFeaturesManager::GetOrCreateCtrBinarizationId(
            const NCatboostOptions::TBinarizationOptions& binarization) const {
        for (ui32 i = 0; i < CtrBinarizationOptions.size(); ++i) {
            if (CtrBinarizationOptions[i] == binarization) {
                return i;
            }
        }
        ui32 id = CtrBinarizationOptions.size();
        CtrBinarizationOptions.push_back(binarization);
        return id;
    }

}
