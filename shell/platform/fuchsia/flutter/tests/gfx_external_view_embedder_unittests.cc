// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/fuchsia/flutter/gfx_external_view_embedder.h"

#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/async-testing/test_loop.h>
#include <lib/async/dispatcher.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/synchronous_interface_ptr.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "flutter/flow/embedded_views.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/time/time_delta.h"
#include "flutter/fml/time/time_point.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkSize.h"
#include "third_party/skia/include/core/SkSurface.h"

#include "fakes/scenic/fake_resources.h"
#include "fakes/scenic/fake_session.h"
#include "flutter/shell/platform/fuchsia/flutter/surface_producer.h"

#include "gmock/gmock.h"  // For EXPECT_THAT and matchers
#include "gtest/gtest.h"

using fuchsia::scenic::scheduling::FramePresentedInfo;
using fuchsia::scenic::scheduling::FuturePresentationTimes;
using fuchsia::scenic::scheduling::PresentReceivedInfo;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::Matcher;
using ::testing::Pointee;
using ::testing::SizeIs;
using ::testing::VariantWith;

namespace flutter_runner::testing {
namespace {

class FakeSurfaceProducerSurface : public SurfaceProducerSurface {
 public:
  explicit FakeSurfaceProducerSurface(scenic::Session& session,
                                      const SkISize& size,
                                      uint32_t buffer_id)
      : session_(session),
        surface_(SkSurface::MakeNull(size.width(), size.height())),
        image_id_(session_.AllocResourceId()),
        buffer_id_(buffer_id) {
    FML_CHECK(buffer_id_ != 0);

    fuchsia::sysmem::BufferCollectionTokenSyncPtr token;
    buffer_binding_ = token.NewRequest();

    session_.RegisterBufferCollection(buffer_id_, std::move(token));
    session_.Enqueue(scenic::NewCreateImage2Cmd(
        image_id_, surface_->width(), surface_->height(), buffer_id_, 0));
  }
  ~FakeSurfaceProducerSurface() override {
    session_.DeregisterBufferCollection(buffer_id_);
    session_.Enqueue(scenic::NewReleaseResourceCmd(image_id_));
  }

  bool IsValid() const override { return true; }

  SkISize GetSize() const override {
    return SkISize::Make(surface_->width(), surface_->height());
  }

  void SetImageId(uint32_t image_id) override { FAIL(); }
  uint32_t GetImageId() override { return image_id_; }

  sk_sp<SkSurface> GetSkiaSurface() const override { return surface_; }

  fuchsia::ui::composition::BufferCollectionImportToken
  GetBufferCollectionImportToken() override {
    return fuchsia::ui::composition::BufferCollectionImportToken{};
  }

  zx::event GetAcquireFence() override { return zx::event{}; }

  zx::event GetReleaseFence() override { return zx::event{}; }

  void SetReleaseImageCallback(
      ReleaseImageCallback release_image_callback) override {}

  size_t AdvanceAndGetAge() override { return 0; }
  bool FlushSessionAcquireAndReleaseEvents() override { return true; }
  void SignalWritesFinished(
      const std::function<void(void)>& on_writes_committed) override {}

 private:
  scenic::Session& session_;

  sk_sp<SkSurface> surface_;

  fidl::InterfaceRequest<fuchsia::sysmem::BufferCollectionToken>
      buffer_binding_;
  FakeResourceId image_id_{kInvalidFakeResourceId};
  uint32_t buffer_id_{0};
};

class FakeSurfaceProducer : public SurfaceProducer {
 public:
  explicit FakeSurfaceProducer(scenic::Session& session) : session_(session) {}
  ~FakeSurfaceProducer() override = default;

  std::unique_ptr<SurfaceProducerSurface> ProduceSurface(
      const SkISize& size) override {
    return std::make_unique<FakeSurfaceProducerSurface>(session_, size,
                                                        buffer_id_++);
  }

  void SubmitSurfaces(
      std::vector<std::unique_ptr<SurfaceProducerSurface>> surfaces) override {}

 private:
  scenic::Session& session_;

  uint32_t buffer_id_{1};
};

struct FakeCompositorLayer {
  enum class LayerType : uint32_t {
    Image,
    View,
  };

  std::shared_ptr<FakeResource> layer_root;

  LayerType layer_type{LayerType::Image};
  size_t layer_index{0};
};

std::string GetCurrentTestName() {
  return ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

zx_koid_t GetKoid(zx_handle_t handle) {
  if (handle == ZX_HANDLE_INVALID) {
    return ZX_KOID_INVALID;
  }

  zx_info_handle_basic_t info;
  zx_status_t status = zx_object_get_info(handle, ZX_INFO_HANDLE_BASIC, &info,
                                          sizeof(info), nullptr, nullptr);
  return status == ZX_OK ? info.koid : ZX_KOID_INVALID;
}

zx_koid_t GetPeerKoid(zx_handle_t handle) {
  if (handle == ZX_HANDLE_INVALID) {
    return ZX_KOID_INVALID;
  }

  zx_info_handle_basic_t info;
  zx_status_t status = zx_object_get_info(handle, ZX_INFO_HANDLE_BASIC, &info,
                                          sizeof(info), nullptr, nullptr);
  return status == ZX_OK ? info.related_koid : ZX_KOID_INVALID;
}

MATCHER_P(MaybeIsEmpty, assert_empty, "") {
  return assert_empty ? ExplainMatchResult(IsEmpty(), arg, result_listener)
                      : ExplainMatchResult(_, arg, result_listener);
}

Matcher<FakeSceneGraph> IsEmptySceneGraph() {
  return FieldsAre(IsEmpty(), IsEmpty(), IsEmpty(), kInvalidFakeResourceId);
}

void AssertRootSceneGraph(const FakeSceneGraph& scene_graph,
                          bool assert_empty) {
  ASSERT_NE(scene_graph.root_view_id, kInvalidFakeResourceId);
  ASSERT_EQ(scene_graph.resource_map.count(scene_graph.root_view_id), 1u);
  auto scene_graph_root =
      scene_graph.resource_map.find(scene_graph.root_view_id);
  ASSERT_THAT(
      scene_graph_root->second,
      Pointee(FieldsAre(
          scene_graph.root_view_id, "", FakeResource::kDefaultEmptyEventMask,
          VariantWith<FakeView>(FieldsAre(
              _, _, _, _,
              ElementsAre(Pointee(FieldsAre(
                  _, "Flutter::MetricsWatcher",
                  fuchsia::ui::gfx::kMetricsEventMask,
                  VariantWith<FakeEntityNode>(FieldsAre(
                      FieldsAre(
                          ElementsAre(Pointee(FieldsAre(
                              _, "Flutter::LayerTree",
                              FakeResource::kDefaultEmptyEventMask,
                              VariantWith<FakeEntityNode>(FieldsAre(
                                  FieldsAre(MaybeIsEmpty(assert_empty),
                                            FakeNode::kDefaultZeroRotation,
                                            FakeNode::kDefaultOneScale,
                                            FakeNode::kDefaultZeroTranslation,
                                            FakeNode::kDefaultZeroAnchor,
                                            FakeNode::kIsHitTestable,
                                            FakeNode::kIsSemanticallyVisible),
                                  IsEmpty()))))),
                          FakeNode::kDefaultZeroRotation,
                          FakeNode::kDefaultOneScale,
                          FakeNode::kDefaultZeroTranslation,
                          FakeNode::kDefaultZeroAnchor,
                          FakeNode::kIsHitTestable,
                          FakeNode::kIsSemanticallyVisible),
                      IsEmpty()))))),
              FakeView::kDebugBoundsDisbaled)))));
}

void ExpectRootSceneGraph(
    const FakeSceneGraph& scene_graph,
    const std::string& debug_name,
    const fuchsia::ui::views::ViewHolderToken& view_holder_token,
    const fuchsia::ui::views::ViewRef& view_ref) {
  AssertRootSceneGraph(scene_graph, true);

  // These are safe to do unchecked due to `AssertRootSceneGraph` above.
  auto root_view_it = scene_graph.resource_map.find(scene_graph.root_view_id);
  auto* root_view_state = std::get_if<FakeView>(&root_view_it->second->state);
  EXPECT_EQ(root_view_state->token, GetPeerKoid(view_holder_token.value.get()));
  EXPECT_EQ(root_view_state->control_ref,
            GetPeerKoid(view_ref.reference.get()));
  EXPECT_EQ(root_view_state->view_ref, GetKoid(view_ref.reference.get()));
  EXPECT_EQ(root_view_state->debug_name, debug_name);
  EXPECT_EQ(scene_graph.resource_map.size(), 3u);
}

void ExpectImageCompositorLayer(const FakeCompositorLayer& layer,
                                const SkISize layer_size) {
  const SkSize float_layer_size =
      SkSize::Make(layer_size.width(), layer_size.height());
  const size_t flutter_layer_index =
      (layer.layer_index + 1) / 2;  // Integer division
  const float views_under_layer_depth =
      flutter_layer_index *
      GfxExternalViewEmbedder::kScenicZElevationForPlatformView;
  const float layer_depth =
      flutter_layer_index *
          GfxExternalViewEmbedder::kScenicZElevationBetweenLayers +
      views_under_layer_depth;
  const bool layer_hit_testable = (flutter_layer_index == 0)
                                      ? FakeNode::kIsHitTestable
                                      : FakeNode::kIsNotHitTestable;
  const float layer_opacity =
      (flutter_layer_index == 0)
          ? GfxExternalViewEmbedder::kBackgroundLayerOpacity / 255.f
          : GfxExternalViewEmbedder::kOverlayLayerOpacity / 255.f;
  EXPECT_EQ(layer.layer_type, FakeCompositorLayer::LayerType::Image);
  EXPECT_EQ(layer.layer_index % 2, 0u);
  EXPECT_THAT(
      layer.layer_root,
      Pointee(FieldsAre(
          _, "Flutter::Layer", FakeResource::kDefaultEmptyEventMask,
          VariantWith<FakeShapeNode>(FieldsAre(
              FieldsAre(IsEmpty(), FakeNode::kDefaultZeroRotation,
                        FakeNode::kDefaultOneScale,
                        std::array<float, 3>{float_layer_size.width() / 2.f,
                                             float_layer_size.height() / 2.f,
                                             -layer_depth},
                        FakeNode::kDefaultZeroAnchor, layer_hit_testable,
                        FakeNode::kIsSemanticallyVisible),
              Pointee(
                  FieldsAre(_, "", FakeResource::kDefaultEmptyEventMask,
                            VariantWith<FakeShape>(
                                FieldsAre(VariantWith<FakeShape::RectangleDef>(
                                    FieldsAre(float_layer_size.width(),
                                              float_layer_size.height())))))),
              Pointee(FieldsAre(
                  _, "", FakeResource::kDefaultEmptyEventMask,
                  VariantWith<FakeMaterial>(FieldsAre(
                      Pointee(FieldsAre(
                          _, "", FakeResource::kDefaultEmptyEventMask,
                          VariantWith<FakeImage>(FieldsAre(
                              VariantWith<FakeImage::Image2Def>(
                                  FieldsAre(_, 0, float_layer_size.width(),
                                            float_layer_size.height())),
                              IsNull())))),
                      std::array<float, 4>{1.f, 1.f, 1.f,
                                           layer_opacity})))))))));
}

void ExpectViewCompositorLayer(const FakeCompositorLayer& layer,
                               const fuchsia::ui::views::ViewToken& view_token,
                               const flutter::EmbeddedViewParams& view_params) {
  const size_t flutter_layer_index =
      (layer.layer_index + 1) / 2;  // Integer division
  const float views_under_layer_depth =
      flutter_layer_index > 0
          ? (flutter_layer_index - 1) *
                GfxExternalViewEmbedder::kScenicZElevationForPlatformView
          : 0.f;
  const float layer_depth =
      flutter_layer_index *
          GfxExternalViewEmbedder::kScenicZElevationBetweenLayers +
      views_under_layer_depth;
  EXPECT_EQ(layer.layer_type, FakeCompositorLayer::LayerType::View);
  EXPECT_EQ(layer.layer_index % 2, 1u);
  EXPECT_THAT(
      layer.layer_root,
      Pointee(FieldsAre(
          _, _ /*"Flutter::PlatformView::OpacityMutator" */,
          FakeResource::kDefaultEmptyEventMask,
          VariantWith<FakeOpacityNode>(FieldsAre(
              FieldsAre(
                  ElementsAre(Pointee(FieldsAre(
                      _, _ /*"Flutter::PlatformView::TransformMutator" */,
                      FakeResource::kDefaultEmptyEventMask,
                      VariantWith<FakeEntityNode>(FieldsAre(
                          FieldsAre(
                              ElementsAre(Pointee(FieldsAre(
                                  _, "", FakeResource::kDefaultEmptyEventMask,
                                  VariantWith<FakeViewHolder>(FieldsAre(
                                      FieldsAre(
                                          IsEmpty(),
                                          FakeNode::kDefaultZeroRotation,
                                          FakeNode::kDefaultOneScale,
                                          FakeNode::kDefaultZeroTranslation,
                                          FakeNode::kDefaultZeroAnchor,
                                          FakeNode::kIsHitTestable,
                                          FakeNode::kIsSemanticallyVisible),
                                      GetPeerKoid(view_token.value.get()),
                                      "Flutter::PlatformView",
                                      fuchsia::ui::gfx::ViewProperties{
                                          .bounding_box =
                                              fuchsia::ui::gfx::BoundingBox{
                                                  .min = {0.f, 0.f, -1000.f},
                                                  .max =
                                                      {view_params.sizePoints()
                                                           .width(),
                                                       view_params.sizePoints()
                                                           .height(),
                                                       0.f},
                                              }},
                                      FakeViewHolder::
                                          kDefaultBoundsColorWhite))))),
                              FakeNode::kDefaultZeroRotation,
                              FakeNode::kDefaultOneScale,
                              std::array<float, 3>{0.f, 0.f, -layer_depth},
                              FakeNode::kDefaultZeroAnchor,
                              FakeNode::kIsHitTestable,
                              FakeNode::kIsSemanticallyVisible),
                          IsEmpty()))))),
                  FakeNode::kDefaultZeroRotation, FakeNode::kDefaultOneScale,
                  FakeNode::kDefaultZeroTranslation,
                  FakeNode::kDefaultZeroAnchor, FakeNode::kIsHitTestable,
                  FakeNode::kIsSemanticallyVisible),
              FakeOpacityNode::kDefaultOneOpacity)))));
}

std::vector<FakeCompositorLayer> ExtractLayersFromSceneGraph(
    const FakeSceneGraph& scene_graph) {
  AssertRootSceneGraph(scene_graph, false);

  // These are safe to do unchecked due to `AssertRootSceneGraph` above.
  auto root_view_it = scene_graph.resource_map.find(scene_graph.root_view_id);
  auto* root_view_state = std::get_if<FakeView>(&root_view_it->second->state);
  auto* metrics_watcher_state =
      std::get_if<FakeEntityNode>(&root_view_state->children[0]->state);
  auto* layer_tree_state = std::get_if<FakeEntityNode>(
      &metrics_watcher_state->node_state.children[0]->state);

  std::vector<FakeCompositorLayer> layers;
  for (auto& layer_resource : layer_tree_state->node_state.children) {
    const size_t layer_index = layers.size();
    const FakeCompositorLayer::LayerType layer_type =
        (layer_index % 2 == 0) ? FakeCompositorLayer::LayerType::Image
                               : FakeCompositorLayer::LayerType::View;
    layers.emplace_back(FakeCompositorLayer{
        .layer_root = layer_resource,
        .layer_type = layer_type,
        .layer_index = layer_index,
    });
  }

  return layers;
}

void DrawSimpleFrame(GfxExternalViewEmbedder& external_view_embedder,
                     SkISize frame_size,
                     float frame_dpr,
                     std::function<void(SkCanvas*)> draw_callback) {
  external_view_embedder.BeginFrame(frame_size, nullptr, frame_dpr, nullptr);
  {
    SkCanvas* root_canvas = external_view_embedder.GetRootCanvas();
    external_view_embedder.PostPrerollAction(nullptr);
    draw_callback(root_canvas);
  }
  external_view_embedder.EndFrame(false, nullptr);
  flutter::SurfaceFrame::FramebufferInfo framebuffer_info;
  external_view_embedder.SubmitFrame(
      nullptr, std::make_unique<flutter::SurfaceFrame>(
                   nullptr, framebuffer_info,
                   [](const flutter::SurfaceFrame& surface_frame,
                      SkCanvas* canvas) { return true; }));
}

void DrawFrameWithView(GfxExternalViewEmbedder& external_view_embedder,
                       SkISize frame_size,
                       float frame_dpr,
                       int view_id,
                       flutter::EmbeddedViewParams& view_params,
                       std::function<void(SkCanvas*)> background_draw_callback,
                       std::function<void(SkCanvas*)> overlay_draw_callback) {
  external_view_embedder.BeginFrame(frame_size, nullptr, frame_dpr, nullptr);
  {
    SkCanvas* root_canvas = external_view_embedder.GetRootCanvas();
    external_view_embedder.PrerollCompositeEmbeddedView(
        view_id, std::make_unique<flutter::EmbeddedViewParams>(view_params));
    external_view_embedder.PostPrerollAction(nullptr);
    background_draw_callback(root_canvas);
    SkCanvas* overlay_canvas =
        external_view_embedder.CompositeEmbeddedView(view_id);
    overlay_draw_callback(overlay_canvas);
  }
  external_view_embedder.EndFrame(false, nullptr);
  flutter::SurfaceFrame::FramebufferInfo framebuffer_info;
  external_view_embedder.SubmitFrame(
      nullptr, std::make_unique<flutter::SurfaceFrame>(
                   nullptr, framebuffer_info,
                   [](const flutter::SurfaceFrame& surface_frame,
                      SkCanvas* canvas) { return true; }));
}

FramePresentedInfo MakeFramePresentedInfoForOnePresent(
    int64_t latched_time,
    int64_t frame_presented_time) {
  std::vector<PresentReceivedInfo> present_infos;
  present_infos.emplace_back();
  present_infos.back().set_present_received_time(0);
  present_infos.back().set_latched_time(0);
  return FramePresentedInfo{
      .actual_presentation_time = 0,
      .presentation_infos = std::move(present_infos),
      .num_presents_allowed = 1,
  };
}

};  // namespace

class GfxExternalViewEmbedderTest
    : public ::testing::Test,
      public fuchsia::ui::scenic::SessionListener {
 protected:
  GfxExternalViewEmbedderTest()
      : session_listener_(this),
        session_subloop_(loop_.StartNewLoop()),
        session_connection_(CreateSessionConnection()),
        fake_surface_producer_(*session_connection_.get()) {}
  ~GfxExternalViewEmbedderTest() override = default;

  async::TestLoop& loop() { return loop_; }

  FakeSession& fake_session() { return fake_session_; }

  FakeSurfaceProducer& fake_surface_producer() {
    return fake_surface_producer_;
  }

  GfxSessionConnection& session_connection() { return session_connection_; }

 private:
  // |fuchsia::ui::scenic::SessionListener|
  void OnScenicError(std::string error) override { FAIL(); }

  // |fuchsia::ui::scenic::SessionListener|
  void OnScenicEvent(std::vector<fuchsia::ui::scenic::Event> events) override {
    FAIL();
  }

  GfxSessionConnection CreateSessionConnection() {
    FML_CHECK(!fake_session_.is_bound());
    FML_CHECK(!session_listener_.is_bound());

    inspect::Node inspect_node =
        inspector_.GetRoot().CreateChild("GfxExternalViewEmbedderTest");

    auto [session, session_listener] =
        fake_session_.Bind(session_subloop_->dispatcher());
    session_listener_.Bind(std::move(session_listener));

    return GfxSessionConnection(
        GetCurrentTestName(), std::move(inspect_node), std::move(session),
        []() { FAIL(); }, [](auto...) {}, 1, fml::TimeDelta::Zero());
  }

  async::TestLoop loop_;  // Must come before FIDL bindings.

  inspect::Inspector inspector_;

  fidl::Binding<fuchsia::ui::scenic::SessionListener> session_listener_;

  std::unique_ptr<async::LoopInterface> session_subloop_;
  FakeSession fake_session_;
  GfxSessionConnection session_connection_;

  FakeSurfaceProducer fake_surface_producer_;
};

TEST_F(GfxExternalViewEmbedderTest, RootScene) {
  const std::string debug_name = GetCurrentTestName();
  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
  auto view_ref_pair = scenic::ViewRefPair::New();
  fuchsia::ui::views::ViewRef view_ref;
  view_ref_pair.view_ref.Clone(&view_ref);

  GfxExternalViewEmbedder external_view_embedder(
      debug_name, std::move(view_token), std::move(view_ref_pair),
      session_connection(), fake_surface_producer());
  EXPECT_EQ(fake_session().debug_name(), "");
  EXPECT_THAT(fake_session().SceneGraph(), IsEmptySceneGraph());

  // Pump the loop; the contents of the initial `Present` should be processed.
  loop().RunUntilIdle();
  EXPECT_EQ(fake_session().debug_name(), debug_name);
  ExpectRootSceneGraph(fake_session().SceneGraph(), debug_name,
                       view_holder_token, view_ref);

  // Fire the `OnFramePresented` event associated with the first `Present`, then
  // pump the loop.  The `OnFramePresented` event is resolved.
  //
  // The scene graph shouldn't change.
  fake_session().FireOnFramePresentedEvent(
      MakeFramePresentedInfoForOnePresent(0, 0));
  loop().RunUntilIdle();
  ExpectRootSceneGraph(fake_session().SceneGraph(), debug_name,
                       view_holder_token, view_ref);
}

TEST_F(GfxExternalViewEmbedderTest, SimpleScene) {
  const std::string debug_name = GetCurrentTestName();
  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
  auto view_ref_pair = scenic::ViewRefPair::New();
  fuchsia::ui::views::ViewRef view_ref;
  view_ref_pair.view_ref.Clone(&view_ref);

  // Create the `GfxExternalViewEmbedder` and pump the message loop until
  // the initial scene graph is setup.
  GfxExternalViewEmbedder external_view_embedder(
      debug_name, std::move(view_token), std::move(view_ref_pair),
      session_connection(), fake_surface_producer());
  loop().RunUntilIdle();
  fake_session().FireOnFramePresentedEvent(
      MakeFramePresentedInfoForOnePresent(0, 0));
  loop().RunUntilIdle();
  ExpectRootSceneGraph(fake_session().SceneGraph(), debug_name,
                       view_holder_token, view_ref);

  // Draw the scene.  The scene graph shouldn't change yet.
  const SkISize frame_size = SkISize::Make(512, 512);
  DrawSimpleFrame(
      external_view_embedder, frame_size, 1.f, [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      });
  ExpectRootSceneGraph(fake_session().SceneGraph(), debug_name,
                       view_holder_token, view_ref);

  // Pump the message loop.  The scene updates should propogate to Scenic.
  loop().RunUntilIdle();
  std::vector<FakeCompositorLayer> compositor_layers =
      ExtractLayersFromSceneGraph(fake_session().SceneGraph());
  EXPECT_EQ(compositor_layers.size(), 1u);
  ExpectImageCompositorLayer(compositor_layers[0], frame_size);
}

TEST_F(GfxExternalViewEmbedderTest, SceneWithOneView) {
  const std::string debug_name = GetCurrentTestName();
  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
  auto view_ref_pair = scenic::ViewRefPair::New();
  fuchsia::ui::views::ViewRef view_ref;
  view_ref_pair.view_ref.Clone(&view_ref);

  // Create the `GfxExternalViewEmbedder` and pump the message loop until
  // the initial scene graph is setup.
  GfxExternalViewEmbedder external_view_embedder(
      debug_name, std::move(view_token), std::move(view_ref_pair),
      session_connection(), fake_surface_producer());
  loop().RunUntilIdle();
  fake_session().FireOnFramePresentedEvent(
      MakeFramePresentedInfoForOnePresent(0, 0));
  loop().RunUntilIdle();
  ExpectRootSceneGraph(fake_session().SceneGraph(), debug_name,
                       view_holder_token, view_ref);

  // Create the view before drawing the scene.
  const SkSize child_view_size = SkSize::Make(256.f, 512.f);
  auto [child_view_token, child_view_holder_token] =
      scenic::ViewTokenPair::New();
  const uint32_t child_view_id = child_view_holder_token.value.get();
  flutter::EmbeddedViewParams child_view_params(SkMatrix::I(), child_view_size,
                                                flutter::MutatorsStack());
  external_view_embedder.CreateView(
      child_view_id, []() {}, [](scenic::ResourceId) {});

  // Draw the scene.  The scene graph shouldn't change yet.
  const SkISize frame_size = SkISize::Make(512, 512);
  DrawFrameWithView(
      external_view_embedder, frame_size, 1.f, child_view_id, child_view_params,
      [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      },
      [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorRED);
        canvas->translate(canvas_size.width() * 3.f / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      });
  ExpectRootSceneGraph(fake_session().SceneGraph(), debug_name,
                       view_holder_token, view_ref);

  // Pump the message loop.  The scene updates should propagate to Scenic.
  loop().RunUntilIdle();
  std::vector<FakeCompositorLayer> compositor_layers =
      ExtractLayersFromSceneGraph(fake_session().SceneGraph());
  EXPECT_EQ(compositor_layers.size(), 3u);
  ExpectImageCompositorLayer(compositor_layers[0], frame_size);
  ExpectViewCompositorLayer(compositor_layers[1], child_view_token,
                            child_view_params);
  ExpectImageCompositorLayer(compositor_layers[2], frame_size);

  // Destroy the view.
  external_view_embedder.DestroyView(child_view_id, [](scenic::ResourceId) {});

  // Pump the message loop.
  loop().RunUntilIdle();
}

}  // namespace flutter_runner::testing
