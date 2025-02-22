// Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/distributed/ps/table/sparse_accessor.h"
#include <gflags/gflags.h>
#include "glog/logging.h"
#include "paddle/fluid/string/string_helper.h"

namespace paddle {
namespace distributed {

int SparseAccessor::Initialize() {
  auto name = _config.embed_sgd_param().name();
  _embed_sgd_rule = CREATE_PSCORE_CLASS(SparseValueSGDRule, name);
  _embed_sgd_rule->load_config(_config.embed_sgd_param(), 1);

  name = _config.embedx_sgd_param().name();
  _embedx_sgd_rule = CREATE_PSCORE_CLASS(SparseValueSGDRule, name);
  _embedx_sgd_rule->load_config(_config.embedx_sgd_param(),
                                _config.embedx_dim());

  sparse_feature_value.embed_sgd_dim = _embed_sgd_rule->dim();
  sparse_feature_value.embedx_dim = _config.embedx_dim();
  sparse_feature_value.embedx_sgd_dim = _embedx_sgd_rule->dim();
  _show_click_decay_rate = _config.ctr_accessor_param().show_click_decay_rate();

  return 0;
}

void SparseAccessor::SetTableInfo(AccessorInfo& info) {
  info.dim = Dim();
  info.size = Size();
  info.select_dim = SelectDim();
  info.select_size = SelectSize();
  info.update_dim = UpdateDim();
  info.update_size = UpdateSize();
  info.mf_size = MFSize();
}

size_t SparseAccessor::GetTableInfo(InfoKey key) {
  switch (key) {
    case DIM:
      return Dim();
    case SIZE:
      return Size();
    case SELECT_DIM:
      return SelectDim();
    case SELECT_SIZE:
      return SelectSize();
    case UPDATE_DIM:
      return UpdateDim();
    case UPDATE_SIZE:
      return UpdateSize();
    case MF_SIZE:
      return MFSize();
    default:
      return 0;
  }
  return 0;
}

size_t SparseAccessor::Dim() { return sparse_feature_value.Dim(); }

size_t SparseAccessor::DimSize(size_t dim) {
  auto embedx_dim = _config.embedx_dim();
  return sparse_feature_value.DimSize(dim, embedx_dim);
}

size_t SparseAccessor::Size() { return sparse_feature_value.Size(); }

size_t SparseAccessor::MFSize() {
  return (_config.embedx_dim() + sparse_feature_value.embedx_sgd_dim) *
         sizeof(float);  // embedx embedx_g2sum
}

// pull value
size_t SparseAccessor::SelectDim() {
  auto embedx_dim = _config.embedx_dim();
  return 1 + embedx_dim;
}

size_t SparseAccessor::SelectDimSize(size_t dim) { return sizeof(float); }

size_t SparseAccessor::SelectSize() { return SelectDim() * sizeof(float); }

// push value
size_t SparseAccessor::UpdateDim() {
  auto embedx_dim = _config.embedx_dim();
  return 4 + embedx_dim;
}

size_t SparseAccessor::UpdateDimSize(size_t dim) { return sizeof(float); }

size_t SparseAccessor::UpdateSize() { return UpdateDim() * sizeof(float); }

bool SparseAccessor::Shrink(float* value) {
  auto base_threshold = _config.ctr_accessor_param().base_threshold();
  auto delta_threshold = _config.ctr_accessor_param().delta_threshold();
  auto delete_after_unseen_days =
      _config.ctr_accessor_param().delete_after_unseen_days();
  auto delete_threshold = _config.ctr_accessor_param().delete_threshold();

  // time_decay first
  sparse_feature_value.Show(value) *= _show_click_decay_rate;
  sparse_feature_value.Click(value) *= _show_click_decay_rate;

  // shrink after
  auto score = show_click_score(sparse_feature_value.Show(value),
                                sparse_feature_value.Click(value));
  auto unseen_days = sparse_feature_value.unseen_days(value);
  if (score < delete_threshold || unseen_days > delete_after_unseen_days) {
    return true;
  }
  return false;
}

bool SparseAccessor::Save(float* value, int param) {
  auto base_threshold = _config.ctr_accessor_param().base_threshold();
  auto delta_threshold = _config.ctr_accessor_param().delta_threshold();
  auto delta_keep_days = _config.ctr_accessor_param().delta_keep_days();
  if (param == 2) {
    delta_threshold = 0;
  }
  switch (param) {
    // save all
    case 0: {
      return true;
    }
    // save xbox delta
    case 1:
    // save xbox base
    case 2: {
      if (show_click_score(sparse_feature_value.Show(value),
                           sparse_feature_value.Click(value)) >=
              base_threshold &&
          sparse_feature_value.delta_score(value) >= delta_threshold &&
          sparse_feature_value.unseen_days(value) <= delta_keep_days) {
        // do this after save, because it must not be modified when retry
        if (param == 2) {
          sparse_feature_value.delta_score(value) = 0;
        }
        return true;
      } else {
        return false;
      }
    }
    // already decayed in shrink
    case 3: {
      // do this after save, because it must not be modified when retry
      // sparse_feature_value.unseen_days(value)++;
      return true;
    }
    // save revert batch_model
    case 5: {
      return true;
    }
    default:
      return true;
  }
}

void SparseAccessor::UpdateStatAfterSave(float* value, int param) {
  auto base_threshold = _config.ctr_accessor_param().base_threshold();
  auto delta_threshold = _config.ctr_accessor_param().delta_threshold();
  auto delta_keep_days = _config.ctr_accessor_param().delta_keep_days();
  if (param == 2) {
    delta_threshold = 0;
  }
  switch (param) {
    case 1: {
      if (show_click_score(sparse_feature_value.Show(value),
                           sparse_feature_value.Click(value)) >=
              base_threshold &&
          sparse_feature_value.delta_score(value) >= delta_threshold &&
          sparse_feature_value.unseen_days(value) <= delta_keep_days) {
        sparse_feature_value.delta_score(value) = 0;
      }
    }
      return;
    case 3: {
      sparse_feature_value.unseen_days(value)++;
    }
      return;
    default:
      return;
  }
}

int32_t SparseAccessor::Create(float** values, size_t num) {
  auto embedx_dim = _config.embedx_dim();
  for (size_t value_item = 0; value_item < num; ++value_item) {
    float* value = values[value_item];
    value[sparse_feature_value.unseen_days_index()] = 0;
    value[sparse_feature_value.delta_score_index()] = 0;
    value[sparse_feature_value.ShowIndex()] = 0;
    value[sparse_feature_value.ClickIndex()] = 0;
    value[sparse_feature_value.SlotIndex()] = -1;
    _embed_sgd_rule->init_value(
        value + sparse_feature_value.Embed_W_Index(),
        value + sparse_feature_value.embed_g2sum_index());
    _embedx_sgd_rule->init_value(
        value + sparse_feature_value.Embedx_W_Index(),
        value + sparse_feature_value.embedx_g2sum_index(), false);
  }
  return 0;
}

bool SparseAccessor::NeedExtendMF(float* value) {
  float show = value[sparse_feature_value.ShowIndex()];
  float click = value[sparse_feature_value.ClickIndex()];
  float score = (show - click) * _config.ctr_accessor_param().nonclk_coeff() +
                click * _config.ctr_accessor_param().click_coeff();
  return score >= _config.embedx_threshold();
}

bool SparseAccessor::HasMF(size_t size) {
  return size > sparse_feature_value.embedx_g2sum_index();
}

// from SparseFeatureValue to SparsePullValue
int32_t SparseAccessor::Select(float** select_values, const float** values,
                               size_t num) {
  auto embedx_dim = _config.embedx_dim();
  for (size_t value_item = 0; value_item < num; ++value_item) {
    float* select_value = select_values[value_item];
    const float* value = values[value_item];
    select_value[SparsePullValue::Embed_W_Index()] =
        value[sparse_feature_value.Embed_W_Index()];
    memcpy(select_value + SparsePullValue::Embedx_W_Index(),
           value + sparse_feature_value.Embedx_W_Index(),
           embedx_dim * sizeof(float));
  }
  return 0;
}

// from SparsePushValue to SparsePushValue
// first dim: item
// second dim: field num
int32_t SparseAccessor::Merge(float** update_values,
                              const float** other_update_values, size_t num) {
  auto embedx_dim = _config.embedx_dim();
  size_t total_dim = SparsePushValue::Dim(embedx_dim);
  for (size_t value_item = 0; value_item < num; ++value_item) {
    float* update_value = update_values[value_item];
    const float* other_update_value = other_update_values[value_item];
    for (auto i = 0u; i < total_dim; ++i) {
      if (i != SparsePushValue::SlotIndex()) {
        update_value[i] += other_update_value[i];
      }
    }
  }
  return 0;
}

// from SparsePushValue to SparseFeatureValue
// first dim: item
// second dim: field num
int32_t SparseAccessor::Update(float** update_values, const float** push_values,
                               size_t num) {
  auto embedx_dim = _config.embedx_dim();
  for (size_t value_item = 0; value_item < num; ++value_item) {
    float* update_value = update_values[value_item];
    const float* push_value = push_values[value_item];
    float push_show = push_value[SparsePushValue::ShowIndex()];
    float push_click = push_value[SparsePushValue::ClickIndex()];
    float slot = push_value[SparsePushValue::SlotIndex()];
    update_value[sparse_feature_value.ShowIndex()] += push_show;
    update_value[sparse_feature_value.ClickIndex()] += push_click;
    update_value[sparse_feature_value.SlotIndex()] = slot;
    update_value[sparse_feature_value.delta_score_index()] +=
        (push_show - push_click) * _config.ctr_accessor_param().nonclk_coeff() +
        push_click * _config.ctr_accessor_param().click_coeff();
    update_value[sparse_feature_value.unseen_days_index()] = 0;
    _embed_sgd_rule->update_value(
        update_value + sparse_feature_value.Embed_W_Index(),
        update_value + sparse_feature_value.embed_g2sum_index(),
        push_value + SparsePushValue::Embed_G_Index());
    _embedx_sgd_rule->update_value(
        update_value + sparse_feature_value.Embedx_W_Index(),
        update_value + sparse_feature_value.embedx_g2sum_index(),
        push_value + SparsePushValue::Embedx_G_Index());
  }
  return 0;
}

bool SparseAccessor::CreateValue(int stage, const float* value) {
  // stage == 0, pull
  // stage == 1, push
  if (stage == 0) {
    return true;
  } else if (stage == 1) {
    // operation
    auto show = SparsePushValue::Show(const_cast<float*>(value));
    auto click = SparsePushValue::Click(const_cast<float*>(value));
    auto score = show_click_score(show, click);
    if (score <= 0) {
      return false;
    }
    if (score >= 1) {
      return true;
    }
    return local_uniform_real_distribution<float>()(local_random_engine()) <
           score;
  } else {
    return true;
  }
}

float SparseAccessor::show_click_score(float show, float click) {
  auto nonclk_coeff = _config.ctr_accessor_param().nonclk_coeff();
  auto click_coeff = _config.ctr_accessor_param().click_coeff();
  return (show - click) * nonclk_coeff + click * click_coeff;
}

std::string SparseAccessor::ParseToString(const float* v, int param) {
  thread_local std::ostringstream os;
  os.clear();
  os.str("");
  os << v[0] << " " << v[1] << " " << v[2] << " " << v[3] << " " << v[4] << " "
     << v[5];
  for (int i = sparse_feature_value.embed_g2sum_index();
       i < sparse_feature_value.Embedx_W_Index(); i++) {
    os << " " << v[i];
  }
  auto show = sparse_feature_value.Show(const_cast<float*>(v));
  auto click = sparse_feature_value.Click(const_cast<float*>(v));
  auto score = show_click_score(show, click);
  if (score >= _config.embedx_threshold() &&
      param > sparse_feature_value.Embedx_W_Index()) {
    for (auto i = sparse_feature_value.Embedx_W_Index();
         i < sparse_feature_value.Dim(); ++i) {
      os << " " << v[i];
    }
  }
  return os.str();
}

int SparseAccessor::ParseFromString(const std::string& str, float* value) {
  int embedx_dim = _config.embedx_dim();

  _embedx_sgd_rule->init_value(
      value + sparse_feature_value.Embedx_W_Index(),
      value + sparse_feature_value.embedx_g2sum_index());
  auto ret = paddle::string::str_to_float(str.data(), value);
  CHECK(ret >= 6) << "expect more than 6 real:" << ret;
  return ret;
}

}  // namespace distributed
}  // namespace paddle
