#pragma once

#include "format.hpp"
#include <cstddef>
#include <cstdint>

namespace tashkeel {

class FlatTreeEnsemble {
public:
    FlatTreeEnsemble() = default;

    bool init(const uint8_t* data, uint32_t byte_size, uint8_t mode = 0) {
        // mode: 0=v2 FlatTreeNode, 1=v3 FlatTreeNodeV3, 2=v5 packed uint32_t
        valid_ = false;
        mode_ = mode;
        if (!data || byte_size < sizeof(TreeEnsembleHeader)) return false;
        header_ = reinterpret_cast<const TreeEnsembleHeader*>(data);
        size_t node_sz = (mode == 2) ? sizeof(uint32_t)
                       : ((mode == 1) ? sizeof(FlatTreeNodeV3) : sizeof(FlatTreeNode));
        size_t expected = sizeof(TreeEnsembleHeader) +
                          static_cast<size_t>(header_->num_trees) * sizeof(uint32_t) +
                          static_cast<size_t>(header_->num_nodes) * node_sz;
        if (byte_size < expected || header_->num_classes == 0 || header_->num_classes > NUM_DIACRITICS) return false;
        tree_roots_ = reinterpret_cast<const uint32_t*>(data + sizeof(TreeEnsembleHeader));
        const uint8_t* np = data + sizeof(TreeEnsembleHeader) + static_cast<size_t>(header_->num_trees) * sizeof(uint32_t);
        nodes_v2_ = nullptr;
        nodes_v3_ = nullptr;
        nodes_v5_ = nullptr;
        if (mode == 2) nodes_v5_ = reinterpret_cast<const uint32_t*>(np);
        else if (mode == 1) nodes_v3_ = reinterpret_cast<const FlatTreeNodeV3*>(np);
        else nodes_v2_ = reinterpret_cast<const FlatTreeNode*>(np);
        valid_ = true;
        return true;
    }

    bool valid() const { return valid_; }
    uint32_t num_classes() const { return valid_ ? header_->num_classes : 0; }

    inline void predict_scores(const int32_t* features, size_t num_features, int32_t* out_scores) const {
        if (!valid_) return;
        const uint32_t nc = header_->num_classes;
        for (uint32_t c = 0; c < nc; ++c) out_scores[c] = header_->base_score_q;
        const uint32_t nt = header_->num_trees;
        const uint32_t total = header_->num_nodes;
        if (mode_ == 2) {
            for (uint32_t t = 0; t < nt; ++t) {
                uint32_t idx = tree_roots_[t];
                while (idx < total) {
                    const uint32_t node = nodes_v5_[idx];
                    if (node >> 31) {
                        uint32_t cid = (node >> 26) & 0x1Fu;
                        if (cid < nc) out_scores[cid] += static_cast<int16_t>(node & 0xFFFFu);
                        break;
                    }
                    uint32_t feat = (node >> 17) & 0x7Fu;
                    int32_t thresh = static_cast<int32_t>((node >> 24) & 0x7Fu) - 8;
                    int32_t f = (feat < num_features) ? features[feat] : -1;
                    idx += (f <= thresh) ? 1u : (node & 0x1FFFFu);
                }
            }
            return;
        }
        if (mode_ == 1) {
            for (uint32_t t = 0; t < nt; ++t) {
                uint32_t idx = tree_roots_[t];
                uint32_t guard = 0;
                while (idx < total && guard++ < 256) {
                    const FlatTreeNodeV3& node = nodes_v3_[idx];
                    uint8_t kind = node.kind_class >> 4;
                    uint8_t cid = node.kind_class & 0x0F;
                    if (kind == NODE_LEAF) {
                        if (cid < nc) out_scores[cid] += node.threshold;
                        break;
                    }
                    int32_t f = (node.feature_idx < num_features) ? features[node.feature_idx] : -1;
                    idx = static_cast<uint32_t>(f <= node.threshold ? node.left_child : node.right_child);
                }
            }
            return;
        }
        for (uint32_t t = 0; t < nt; ++t) {
            uint32_t idx = tree_roots_[t];
            uint32_t guard = 0;
            while (idx < total && guard++ < 256) {
                const FlatTreeNode& node = nodes_v2_[idx];
                if (node.kind == NODE_LEAF) {
                    if (node.class_id < nc) out_scores[node.class_id] += node.threshold;
                    break;
                }
                int32_t f = (node.feature_idx < num_features) ? features[node.feature_idx] : -1;
                bool left;
                if (node.kind == NODE_CAT) {
                    left = (f >= 0 && f < 64) && ((node.cat_mask >> f) & 1ULL);
                } else {
                    left = f <= node.threshold;
                }
                idx = static_cast<uint32_t>(left ? node.left_child : node.right_child);
            }
        }
    }

private:
    const TreeEnsembleHeader* header_ = nullptr;
    const uint32_t* tree_roots_ = nullptr;
    const FlatTreeNode* nodes_v2_ = nullptr;
    const FlatTreeNodeV3* nodes_v3_ = nullptr;
    const uint32_t* nodes_v5_ = nullptr;
    uint8_t mode_ = 0;
    bool valid_ = false;
};

}
