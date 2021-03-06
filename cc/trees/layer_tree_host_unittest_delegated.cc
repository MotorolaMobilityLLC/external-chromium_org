// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/trees/layer_tree_host.h"

#include "base/bind.h"
#include "cc/layers/delegated_renderer_layer.h"
#include "cc/layers/delegated_renderer_layer_client.h"
#include "cc/layers/delegated_renderer_layer_impl.h"
#include "cc/output/compositor_frame.h"
#include "cc/output/compositor_frame_ack.h"
#include "cc/output/delegated_frame_data.h"
#include "cc/quads/shared_quad_state.h"
#include "cc/quads/texture_draw_quad.h"
#include "cc/test/fake_delegated_renderer_layer.h"
#include "cc/test/fake_delegated_renderer_layer_impl.h"
#include "cc/test/fake_output_surface.h"
#include "cc/test/layer_tree_test.h"
#include "cc/trees/layer_tree_impl.h"
#include "gpu/GLES2/gl2extchromium.h"

namespace cc {
namespace {

// These tests deal with delegated renderer layers.
class LayerTreeHostDelegatedTest : public LayerTreeTest {
 protected:
  scoped_ptr<DelegatedFrameData> CreateFrameData(gfx::Rect root_output_rect,
                                                 gfx::Rect root_damage_rect) {
    scoped_ptr<DelegatedFrameData> frame(new DelegatedFrameData);

    scoped_ptr<RenderPass> root_pass(RenderPass::Create());
    root_pass->SetNew(RenderPass::Id(1, 1),
                      root_output_rect,
                      root_damage_rect,
                      gfx::Transform());
    frame->render_pass_list.push_back(root_pass.Pass());
    return frame.Pass();
  }

  scoped_ptr<DelegatedFrameData> CreateInvalidFrameData(
      gfx::Rect root_output_rect,
      gfx::Rect root_damage_rect) {
    scoped_ptr<DelegatedFrameData> frame(new DelegatedFrameData);

    scoped_ptr<RenderPass> root_pass(RenderPass::Create());
    root_pass->SetNew(RenderPass::Id(1, 1),
                      root_output_rect,
                      root_damage_rect,
                      gfx::Transform());

    scoped_ptr<SharedQuadState> shared_quad_state = SharedQuadState::Create();

    gfx::Rect rect = root_output_rect;
    gfx::Rect opaque_rect = root_output_rect;
    // An invalid resource id! The resource isn't part of the frame.
    unsigned resource_id = 5;
    bool premultiplied_alpha = false;
    gfx::PointF uv_top_left = gfx::PointF(0.f, 0.f);
    gfx::PointF uv_bottom_right = gfx::PointF(1.f, 1.f);
    SkColor background_color = 0;
    float vertex_opacity[4] = {1.f, 1.f, 1.f, 1.f};
    bool flipped = false;

    scoped_ptr<TextureDrawQuad> invalid_draw_quad = TextureDrawQuad::Create();
    invalid_draw_quad->SetNew(shared_quad_state.get(),
                              rect,
                              opaque_rect,
                              resource_id,
                              premultiplied_alpha,
                              uv_top_left,
                              uv_bottom_right,
                              background_color,
                              vertex_opacity,
                              flipped);
    root_pass->quad_list.push_back(invalid_draw_quad.PassAs<DrawQuad>());

    root_pass->shared_quad_state_list.push_back(shared_quad_state.Pass());

    frame->render_pass_list.push_back(root_pass.Pass());
    return frame.Pass();
  }

  void AddTransferableResource(DelegatedFrameData* frame,
                               ResourceProvider::ResourceId resource_id) {
    TransferableResource resource;
    resource.id = resource_id;
    frame->resource_list.push_back(resource);
  }

  void AddTextureQuad(DelegatedFrameData* frame,
                      ResourceProvider::ResourceId resource_id) {
    scoped_ptr<SharedQuadState> sqs = SharedQuadState::Create();
    scoped_ptr<TextureDrawQuad> quad = TextureDrawQuad::Create();
    float vertex_opacity[4] = { 1.f, 1.f, 1.f, 1.f };
    quad->SetNew(sqs.get(),
                 gfx::Rect(0, 0, 10, 10),
                 gfx::Rect(0, 0, 10, 10),
                 resource_id,
                 false,
                 gfx::PointF(0.f, 0.f),
                 gfx::PointF(1.f, 1.f),
                 SK_ColorTRANSPARENT,
                 vertex_opacity,
                 false);
    frame->render_pass_list[0]->shared_quad_state_list.push_back(sqs.Pass());
    frame->render_pass_list[0]->quad_list.push_back(quad.PassAs<DrawQuad>());
  }

  scoped_ptr<DelegatedFrameData> CreateEmptyFrameData() {
    scoped_ptr<DelegatedFrameData> frame(new DelegatedFrameData);
    return frame.Pass();
  }


  static ResourceProvider::ResourceId AppendResourceId(
      std::vector<ResourceProvider::ResourceId>* resources_in_last_sent_frame,
      ResourceProvider::ResourceId resource_id) {
    resources_in_last_sent_frame->push_back(resource_id);
    return resource_id;
  }

  void ReturnUnusedResourcesFromParent(LayerTreeHostImpl* host_impl) {
    DelegatedFrameData* delegated_frame_data =
        output_surface()->last_sent_frame().delegated_frame_data.get();
    if (!delegated_frame_data)
      return;

    std::vector<ResourceProvider::ResourceId> resources_in_last_sent_frame;
    for (size_t i = 0; i < delegated_frame_data->render_pass_list.size(); ++i) {
      RenderPass* pass = delegated_frame_data->render_pass_list.at(i);
      for (size_t j = 0; j < pass->quad_list.size(); ++j) {
        DrawQuad* quad = pass->quad_list[j];
        quad->IterateResources(base::Bind(&AppendResourceId,
                                          &resources_in_last_sent_frame));
      }
    }

    std::vector<ResourceProvider::ResourceId> resources_to_return;

    const TransferableResourceArray& resources_held_by_parent =
        output_surface()->resources_held_by_parent();
    for (size_t i = 0; i < resources_held_by_parent.size(); ++i) {
      ResourceProvider::ResourceId resource_in_parent =
          resources_held_by_parent[i].id;
      bool resource_in_parent_is_not_part_of_frame =
          std::find(resources_in_last_sent_frame.begin(),
                    resources_in_last_sent_frame.end(),
                    resource_in_parent) == resources_in_last_sent_frame.end();
      if (resource_in_parent_is_not_part_of_frame)
        resources_to_return.push_back(resource_in_parent);
    }

    if (resources_to_return.empty())
      return;

    CompositorFrameAck ack;
    for (size_t i = 0; i < resources_to_return.size(); ++i)
      output_surface()->ReturnResource(resources_to_return[i], &ack);
    host_impl->OnSwapBuffersComplete(&ack);
  }
};

class LayerTreeHostDelegatedTestCaseSingleDelegatedLayer
    : public LayerTreeHostDelegatedTest,
      public DelegatedRendererLayerClient {
 public:
  virtual void SetupTree() OVERRIDE {
    root_ = Layer::Create();
    root_->SetAnchorPoint(gfx::PointF());
    root_->SetBounds(gfx::Size(10, 10));

    delegated_ = FakeDelegatedRendererLayer::Create(this);
    delegated_->SetAnchorPoint(gfx::PointF());
    delegated_->SetBounds(gfx::Size(10, 10));
    delegated_->SetIsDrawable(true);

    root_->AddChild(delegated_);
    layer_tree_host()->SetRootLayer(root_);
    LayerTreeHostDelegatedTest::SetupTree();
  }

  virtual void BeginTest() OVERRIDE {
    PostSetNeedsCommitToMainThread();
  }

  virtual void AfterTest() OVERRIDE {}

  virtual void DidCommitFrameData() OVERRIDE {}

 protected:
  scoped_refptr<Layer> root_;
  scoped_refptr<DelegatedRendererLayer> delegated_;
};

class LayerTreeHostDelegatedTestClientDidCommitCallback
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  LayerTreeHostDelegatedTestClientDidCommitCallback()
      : LayerTreeHostDelegatedTestCaseSingleDelegatedLayer(),
        num_did_commit_frame_data_(0) {}

  virtual void DidCommit() OVERRIDE {
    if (TestEnded())
      return;

    EXPECT_EQ(1, num_did_commit_frame_data_);
    EndTest();
  }

  virtual void BeginTest() OVERRIDE {
    delegated_->SetFrameData(CreateFrameData(gfx::Rect(0, 0, 1, 1),
                                             gfx::Rect(0, 0, 1, 1)));
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommitFrameData() OVERRIDE {
    num_did_commit_frame_data_++;
  }

 protected:
  int num_did_commit_frame_data_;
};

SINGLE_AND_MULTI_THREAD_TEST_F(
    LayerTreeHostDelegatedTestClientDidCommitCallback);

class LayerTreeHostDelegatedTestCreateChildId
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  LayerTreeHostDelegatedTestCreateChildId()
      : LayerTreeHostDelegatedTestCaseSingleDelegatedLayer(),
        num_activates_(0),
        did_reset_child_id_(false) {}

  virtual void DidCommit() OVERRIDE {
    if (TestEnded())
      return;
    delegated_->SetFrameData(CreateFrameData(gfx::Rect(0, 0, 1, 1),
                                             gfx::Rect(0, 0, 1, 1)));
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    WebKit::WebGraphicsContext3D* context =
        host_impl->resource_provider()->GraphicsContext3D();

    ++num_activates_;
    switch (num_activates_) {
      case 2:
        EXPECT_TRUE(delegated_impl->ChildId());
        EXPECT_FALSE(did_reset_child_id_);

        context->loseContextCHROMIUM(GL_GUILTY_CONTEXT_RESET_ARB,
                                     GL_INNOCENT_CONTEXT_RESET_ARB);
        break;
      case 3:
        EXPECT_TRUE(delegated_impl->ChildId());
        EXPECT_TRUE(did_reset_child_id_);
        EndTest();
        break;
    }
  }

  virtual void InitializedRendererOnThread(LayerTreeHostImpl* host_impl,
                                           bool success) OVERRIDE {
    EXPECT_TRUE(success);

    if (num_activates_ < 2)
      return;

    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    EXPECT_EQ(2, num_activates_);
    EXPECT_FALSE(delegated_impl->ChildId());
    did_reset_child_id_ = true;
  }

  virtual void AfterTest() OVERRIDE {}

 protected:
  int num_activates_;
  bool did_reset_child_id_;
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestCreateChildId);

class LayerTreeHostDelegatedTestLayerUsesFrameDamage
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  LayerTreeHostDelegatedTestLayerUsesFrameDamage()
      : LayerTreeHostDelegatedTestCaseSingleDelegatedLayer(),
        first_draw_for_source_frame_(true) {}

  virtual void DidCommit() OVERRIDE {
    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // The first time the layer gets a frame the whole layer should be
        // damaged.
        delegated_->SetFrameData(CreateFrameData(gfx::Rect(0, 0, 1, 1),
                                                 gfx::Rect(0, 0, 1, 1)));
        break;
      case 2:
        // Should create a total amount of gfx::Rect(2, 2, 10, 6) damage.
        // The frame size is 20x20 while the layer is 10x10, so this should
        // produce a gfx::Rect(1, 1, 5, 3) damage rect.
        delegated_->SetFrameData(CreateFrameData(gfx::Rect(0, 0, 20, 20),
                                                 gfx::Rect(2, 2, 5, 5)));
        delegated_->SetFrameData(CreateFrameData(gfx::Rect(0, 0, 20, 20),
                                                 gfx::Rect(7, 2, 5, 6)));
        break;
      case 3:
        // Should create zero damage.
        layer_tree_host()->SetNeedsCommit();
        break;
      case 4:
        // Should damage the full viewport.
        delegated_->SetBounds(gfx::Size(2, 2));
        break;
      case 5:
        // Should create zero damage.
        layer_tree_host()->SetNeedsCommit();
        break;
      case 6:
        // Should damage the full layer.
        delegated_->SetBounds(gfx::Size(6, 6));
        delegated_->SetFrameData(CreateFrameData(gfx::Rect(0, 0, 5, 5),
                                                 gfx::Rect(1, 1, 2, 2)));
        break;
      case 7:
        // Should create zero damage.
        layer_tree_host()->SetNeedsCommit();
        break;
      case 8:
        // Should damage the full layer.
        delegated_->SetDisplaySize(gfx::Size(10, 10));
        break;
      case 9:
        // Should create zero damage.
        layer_tree_host()->SetNeedsCommit();
        break;
      case 10:
        // Setting an empty frame should damage the whole layer the
        // first time.
        delegated_->SetFrameData(CreateEmptyFrameData());
        break;
      case 11:
        // Setting an empty frame shouldn't damage anything after the
        // first time.
        delegated_->SetFrameData(CreateEmptyFrameData());
        break;
      case 12:
        // Having valid content to display agains should damage the whole layer.
        delegated_->SetFrameData(CreateFrameData(gfx::Rect(0, 0, 10, 10),
                                                 gfx::Rect(5, 5, 1, 1)));
        break;
      case 13:
        // An invalid frame isn't used, so it should not cause damage.
        delegated_->SetFrameData(CreateInvalidFrameData(gfx::Rect(0, 0, 10, 10),
                                                        gfx::Rect(5, 5, 1, 1)));
        break;
      case 14:
        // Should create gfx::Rect(1, 1, 2, 2) of damage. The frame size is
        // 5x5 and the display size is now set to 10x10, so this should result
        // in a gfx::Rect(2, 2, 4, 4) damage rect.
        delegated_->SetFrameData(CreateFrameData(gfx::Rect(0, 0, 5, 5),
                                                 gfx::Rect(1, 1, 2, 2)));
        break;
      case 15:
        // Should create zero damage.
        layer_tree_host()->SetNeedsCommit();
        break;
    }
    first_draw_for_source_frame_ = true;
  }

  virtual bool PrepareToDrawOnThread(LayerTreeHostImpl* host_impl,
                                     LayerTreeHostImpl::FrameData* frame,
                                     bool result) OVERRIDE {
    EXPECT_TRUE(result);

    if (!first_draw_for_source_frame_)
      return result;

    gfx::RectF damage_rect;
    if (!frame->has_no_damage) {
      damage_rect = frame->render_passes.back()->damage_rect;
    } else {
      // If there is no damage, then we have no render passes to send.
      EXPECT_TRUE(frame->render_passes.empty());
    }

    switch (host_impl->active_tree()->source_frame_number()) {
      case 0:
        // First frame is damaged because of viewport resize.
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 10.f, 10.f).ToString(),
                  damage_rect.ToString());
        break;
      case 1:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 10.f, 10.f).ToString(),
                  damage_rect.ToString());
        break;
      case 2:
        EXPECT_EQ(gfx::RectF(1.f, 1.f, 5.f, 3.f).ToString(),
                  damage_rect.ToString());
        break;
      case 3:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 0.f, 0.f).ToString(),
                  damage_rect.ToString());
        break;
      case 4:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 10.f, 10.f).ToString(),
                  damage_rect.ToString());
        break;
      case 5:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 0.f, 0.f).ToString(),
                  damage_rect.ToString());
        break;
      case 6:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 6.f, 6.f).ToString(),
                  damage_rect.ToString());
        break;
      case 7:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 0.f, 0.f).ToString(),
                  damage_rect.ToString());
        break;
      case 8:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 6.f, 6.f).ToString(),
                  damage_rect.ToString());
        break;
      case 9:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 0.f, 0.f).ToString(),
                  damage_rect.ToString());
        break;
      case 10:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 6.f, 6.f).ToString(),
                  damage_rect.ToString());
        break;
      case 11:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 0.f, 0.f).ToString(),
                  damage_rect.ToString());
        break;
      case 12:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 6.f, 6.f).ToString(),
                  damage_rect.ToString());
        break;
      case 13:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 0.f, 0.f).ToString(),
                  damage_rect.ToString());
        break;
      case 14:
        EXPECT_EQ(gfx::RectF(2.f, 2.f, 4.f, 4.f).ToString(),
                  damage_rect.ToString());
        break;
      case 15:
        EXPECT_EQ(gfx::RectF(0.f, 0.f, 0.f, 0.f).ToString(),
                  damage_rect.ToString());
        EndTest();
        break;
    }

    return result;
  }

 protected:
  bool first_draw_for_source_frame_;
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestLayerUsesFrameDamage);

class LayerTreeHostDelegatedTestMergeResources
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    // Push two frames to the delegated renderer layer with no commit between.

    // The first frame has resource 999.
    scoped_ptr<DelegatedFrameData> frame1 =
        CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
    AddTextureQuad(frame1.get(), 999);
    AddTransferableResource(frame1.get(), 999);
    delegated_->SetFrameData(frame1.Pass());

    // The second frame uses resource 999 still, but also adds 555.
    scoped_ptr<DelegatedFrameData> frame2 =
        CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
    AddTextureQuad(frame2.get(), 999);
    AddTextureQuad(frame2.get(), 555);
    AddTransferableResource(frame2.get(), 555);
    delegated_->SetFrameData(frame2.Pass());

    PostSetNeedsCommitToMainThread();
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    // Both frames' resources should be in the parent's resource provider.
    EXPECT_EQ(2u, map.size());
    EXPECT_EQ(1u, map.count(999));
    EXPECT_EQ(1u, map.count(555));

    EXPECT_EQ(2u, delegated_impl->Resources().size());
    EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));
    EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));

    EndTest();
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestMergeResources);

class LayerTreeHostDelegatedTestRemapResourcesInQuads
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    // Generate a frame with two resources in it.
    scoped_ptr<DelegatedFrameData> frame =
        CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
    AddTextureQuad(frame.get(), 999);
    AddTransferableResource(frame.get(), 999);
    AddTextureQuad(frame.get(), 555);
    AddTransferableResource(frame.get(), 555);
    delegated_->SetFrameData(frame.Pass());

    PostSetNeedsCommitToMainThread();
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    // The frame's resource should be in the parent's resource provider.
    EXPECT_EQ(2u, map.size());
    EXPECT_EQ(1u, map.count(999));
    EXPECT_EQ(1u, map.count(555));

    ResourceProvider::ResourceId parent_resource_id1 = map.find(999)->second;
    EXPECT_NE(parent_resource_id1, 999u);
    ResourceProvider::ResourceId parent_resource_id2 = map.find(555)->second;
    EXPECT_NE(parent_resource_id2, 555u);

    // The resources in the quads should be remapped to the parent's namespace.
    const TextureDrawQuad* quad1 = TextureDrawQuad::MaterialCast(
        delegated_impl->RenderPassesInDrawOrder()[0]->quad_list[0]);
    EXPECT_EQ(parent_resource_id1, quad1->resource_id);
    const TextureDrawQuad* quad2 = TextureDrawQuad::MaterialCast(
        delegated_impl->RenderPassesInDrawOrder()[0]->quad_list[1]);
    EXPECT_EQ(parent_resource_id2, quad2->resource_id);

    EndTest();
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestRemapResourcesInQuads);

class LayerTreeHostDelegatedTestReturnUnusedResources
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommitAndDrawFrame() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // Generate a frame with two resources in it.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 3:
        // All of the resources are in use.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // Keep using 999 but stop using 555.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTextureQuad(frame.get(), 444);
        AddTransferableResource(frame.get(), 444);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 4:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 5:
        // 555 is no longer in use.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(1u, resources.size());
        EXPECT_EQ(555u, resources[0].id);

        // Stop using any resources.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        delegated_->SetFrameData(frame.Pass());
        break;
      case 6:
        // Postpone collecting resources for a frame. They should still be there
        // the next frame.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 7:
        // 444 and 999 are no longer in use.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(2u, resources.size());
        if (resources[0].id == 999) {
          EXPECT_EQ(999u, resources[0].id);
          EXPECT_EQ(444u, resources[1].id);
        } else {
          EXPECT_EQ(444u, resources[0].id);
          EXPECT_EQ(999u, resources[1].id);
        }
        EndTest();
        break;
    }

    // Resource are never immediately released.
    TransferableResourceArray empty_resources;
    delegated_->TakeUnusedResourcesForChildCompositor(&empty_resources);
    EXPECT_TRUE(empty_resources.empty());
  }

  virtual void SwapBuffersOnThread(LayerTreeHostImpl* host_impl,
                                   bool result) OVERRIDE {
    ReturnUnusedResourcesFromParent(host_impl);
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(
    LayerTreeHostDelegatedTestReturnUnusedResources);

class LayerTreeHostDelegatedTestReusedResources
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommitAndDrawFrame() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // Generate a frame with some resources in it.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        AddTextureQuad(frame.get(), 444);
        AddTransferableResource(frame.get(), 444);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 3:
        // All of the resources are in use.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // Keep using 999 but stop using 555 and 444.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        delegated_->SetFrameData(frame.Pass());

        // Resource are not immediately released.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // Now using 555 and 444 again, but not 999.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 555);
        AddTextureQuad(frame.get(), 444);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 4:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 5:
        // The 999 resource is the only unused one.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(1u, resources.size());
        EXPECT_EQ(999u, resources[0].id);
        EndTest();
        break;
    }
  }

  virtual void SwapBuffersOnThread(LayerTreeHostImpl* host_impl,
                                   bool result) OVERRIDE {
    ReturnUnusedResourcesFromParent(host_impl);
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestReusedResources);

class LayerTreeHostDelegatedTestFrameBeforeAck
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommitAndDrawFrame() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // Generate a frame with some resources in it.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        AddTextureQuad(frame.get(), 444);
        AddTransferableResource(frame.get(), 444);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 3:
        // All of the resources are in use.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // Keep using 999 but stop using 555 and 444.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        delegated_->SetFrameData(frame.Pass());

        // Resource are not immediately released.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // The parent compositor (this one) does a commit.
        break;
      case 4:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 5:
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(2u, resources.size());
        if (resources[0].id == 555) {
          EXPECT_EQ(555u, resources[0].id);
          EXPECT_EQ(444u, resources[1].id);
        } else {
          EXPECT_EQ(444u, resources[0].id);
          EXPECT_EQ(555u, resources[1].id);
        }

        // The child compositor sends a frame before receiving an for the
        // second frame. It uses 999, 444, and 555 again.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTextureQuad(frame.get(), 444);
        delegated_->SetFrameData(frame.Pass());
        break;
    }
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    if (host_impl->active_tree()->source_frame_number() != 5)
      return;

    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    // The bad frame should be dropped. So we should only have one quad (the
    // one with resource 999) on the impl tree. And only 999 will be present
    // in the parent's resource provider.
    EXPECT_EQ(1u, map.size());
    EXPECT_EQ(1u, map.count(999));

    EXPECT_EQ(1u, delegated_impl->Resources().size());
    EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));

    const RenderPass* pass = delegated_impl->RenderPassesInDrawOrder()[0];
    EXPECT_EQ(1u, pass->quad_list.size());
    const TextureDrawQuad* quad = TextureDrawQuad::MaterialCast(
        pass->quad_list[0]);
    EXPECT_EQ(map.find(999)->second, quad->resource_id);

    EndTest();
  }

  virtual void SwapBuffersOnThread(LayerTreeHostImpl* host_impl,
                                   bool result) OVERRIDE {
    ReturnUnusedResourcesFromParent(host_impl);
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestFrameBeforeAck);

class LayerTreeHostDelegatedTestFrameBeforeTakeResources
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommitAndDrawFrame() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // Generate a frame with some resources in it.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        AddTextureQuad(frame.get(), 444);
        AddTransferableResource(frame.get(), 444);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 3:
        // All of the resources are in use.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // Keep using 999 but stop using 555 and 444.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        delegated_->SetFrameData(frame.Pass());

        // Resource are not immediately released.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // The parent compositor (this one) does a commit.
        break;
      case 4:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 5:
        // The child compositor sends a frame before taking resources back
        // from the previous commit. This frame makes use of the resources 555
        // and 444, which were just released during commit.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTextureQuad(frame.get(), 444);
        delegated_->SetFrameData(frame.Pass());

        // The resources are used by the new frame so are not returned.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());
        break;
      case 6:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 7:
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());
        EndTest();
        break;
    }
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    if (host_impl->active_tree()->source_frame_number() != 5)
      return;

    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    // The third frame has all of the resources in it again, the delegated
    // renderer layer should continue to own the resources for it.
    EXPECT_EQ(3u, map.size());
    EXPECT_EQ(1u, map.count(999));
    EXPECT_EQ(1u, map.count(555));
    EXPECT_EQ(1u, map.count(444));

    EXPECT_EQ(3u, delegated_impl->Resources().size());
    EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));
    EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));
    EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(444)->second));

    const RenderPass* pass = delegated_impl->RenderPassesInDrawOrder()[0];
    EXPECT_EQ(3u, pass->quad_list.size());
    const TextureDrawQuad* quad1 = TextureDrawQuad::MaterialCast(
        pass->quad_list[0]);
    EXPECT_EQ(map.find(999)->second, quad1->resource_id);
    const TextureDrawQuad* quad2 = TextureDrawQuad::MaterialCast(
        pass->quad_list[1]);
    EXPECT_EQ(map.find(555)->second, quad2->resource_id);
    const TextureDrawQuad* quad3 = TextureDrawQuad::MaterialCast(
        pass->quad_list[2]);
    EXPECT_EQ(map.find(444)->second, quad3->resource_id);
  }

  virtual void SwapBuffersOnThread(LayerTreeHostImpl* host_impl,
                                   bool result) OVERRIDE {
    ReturnUnusedResourcesFromParent(host_impl);
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(
    LayerTreeHostDelegatedTestFrameBeforeTakeResources);

class LayerTreeHostDelegatedTestBadFrame
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommitAndDrawFrame() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // Generate a frame with some resources in it.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 3:
        // All of the resources are in use.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // Generate a bad frame with a resource the layer doesn't have. The
        // 885 and 775 resources are unknown, while ownership of the legit 444
        // resource is passed in here. The bad frame does not use any of the
        // previous resources, 999 or 555.
        // A bad quad is present both before and after the good quad.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 885);
        AddTextureQuad(frame.get(), 444);
        AddTransferableResource(frame.get(), 444);
        AddTextureQuad(frame.get(), 775);
        delegated_->SetFrameData(frame.Pass());

        // The parent compositor (this one) does a commit.
        break;
      case 4:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 5:
        // The bad frame's resource is given back to the child compositor.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(1u, resources.size());
        EXPECT_EQ(444u, resources[0].id);

        // Now send a good frame with 999 again.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 6:
        // Retrieve unused resources to the main thread.
        // TODO(danakj): Shouldn't need to commit to get resources.
        layer_tree_host()->SetNeedsCommit();
        return;
      case 7:
        // The unused 555 from the last good frame is now released.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(1u, resources.size());
        EXPECT_EQ(555u, resources[0].id);

        EndTest();
        break;
    }
  }

  virtual void SwapBuffersOnThread(LayerTreeHostImpl* host_impl,
                                   bool result) OVERRIDE {
    if (host_impl->active_tree()->source_frame_number() < 1)
      return;

    ReturnUnusedResourcesFromParent(host_impl);

    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    switch (host_impl->active_tree()->source_frame_number()) {
      case 1: {
        // We have the first good frame with just 990 and 555 in it.
        // layer.
        EXPECT_EQ(2u, map.size());
        EXPECT_EQ(1u, map.count(999));
        EXPECT_EQ(1u, map.count(555));

        EXPECT_EQ(2u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));

        const RenderPass* pass = delegated_impl->RenderPassesInDrawOrder()[0];
        EXPECT_EQ(2u, pass->quad_list.size());
        const TextureDrawQuad* quad1 = TextureDrawQuad::MaterialCast(
            pass->quad_list[0]);
        EXPECT_EQ(map.find(999)->second, quad1->resource_id);
        const TextureDrawQuad* quad2 = TextureDrawQuad::MaterialCast(
            pass->quad_list[1]);
        EXPECT_EQ(map.find(555)->second, quad2->resource_id);
        break;
      }
      case 3: {
        // We only keep resources from the last valid frame.
        EXPECT_EQ(2u, map.size());
        EXPECT_EQ(1u, map.count(999));
        EXPECT_EQ(1u, map.count(555));

        EXPECT_EQ(2u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));

        // The bad frame is dropped though, we still have the frame with 999 and
        // 555 in it.
        const RenderPass* pass = delegated_impl->RenderPassesInDrawOrder()[0];
        EXPECT_EQ(2u, pass->quad_list.size());
        const TextureDrawQuad* quad1 = TextureDrawQuad::MaterialCast(
            pass->quad_list[0]);
        EXPECT_EQ(map.find(999)->second, quad1->resource_id);
        const TextureDrawQuad* quad2 = TextureDrawQuad::MaterialCast(
            pass->quad_list[1]);
        EXPECT_EQ(map.find(555)->second, quad2->resource_id);
        break;
      }
      case 5:
        // Resources given to our parent compositor will be returned now, but
        // the DelegatedRendererLayerImpl doesn't know about it until the next
        // commit.
        // TODO(danakj): Shouldn't need a commit to return resources to the
        // DelegatedRendererLayerImpl or to the main thread.
        break;
      case 6: {
        // We have the new good frame with just 999 in it.
        EXPECT_EQ(1u, map.size());
        EXPECT_EQ(1u, map.count(999));

        EXPECT_EQ(1u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));

        const RenderPass* pass = delegated_impl->RenderPassesInDrawOrder()[0];
        EXPECT_EQ(1u, pass->quad_list.size());
        const TextureDrawQuad* quad1 = TextureDrawQuad::MaterialCast(
            pass->quad_list[0]);
        EXPECT_EQ(map.find(999)->second, quad1->resource_id);
        break;
      }
    }
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestBadFrame);

class LayerTreeHostDelegatedTestUnnamedResource
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommit() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // This frame includes two resources in it, but only uses one.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        // The unused resource should be returned.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(1u, resources.size());
        EXPECT_EQ(999u, resources[0].id);

        EndTest();
        break;
    }
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    if (host_impl->active_tree()->source_frame_number() != 1)
      return;

    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    // The layer only held on to the resource that was used.
    EXPECT_EQ(1u, map.size());
    EXPECT_EQ(1u, map.count(555));

    EXPECT_EQ(1u, delegated_impl->Resources().size());
    EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestUnnamedResource);

class LayerTreeHostDelegatedTestDontLeakResource
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommit() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // This frame includes two resources in it.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());

        // But then we immediately stop using 999.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        // The unused resource should be returned.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(1u, resources.size());
        EXPECT_EQ(999u, resources[0].id);

        EndTest();
        break;
    }
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    if (host_impl->active_tree()->source_frame_number() != 1)
      return;

    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    // The layer only held on to the resource that was used.
    EXPECT_EQ(1u, map.size());
    EXPECT_EQ(1u, map.count(555));

    EXPECT_EQ(1u, delegated_impl->Resources().size());
    EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestDontLeakResource);

class LayerTreeHostDelegatedTestResourceSentToParent
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void DidCommitAndDrawFrame() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        // This frame includes two resources in it.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // 999 is in use in the grandparent compositor, generate a frame without
        // it present.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 3:
        // Since 999 is in the grandparent it is not returned.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // The impl side will get back the resource at some point.
        // TODO(danakj): The test should work without this.
        layer_tree_host()->SetNeedsCommit();
        break;
      case 4:
        // 999 was returned from the grandparent and could be released.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(1u, resources.size());
        EXPECT_EQ(999u, resources[0].id);

        EndTest();
        break;
    }
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    if (host_impl->active_tree()->source_frame_number() < 1)
      return;

    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    switch (host_impl->active_tree()->source_frame_number()) {
      case 1: {
        EXPECT_EQ(2u, map.size());
        EXPECT_EQ(1u, map.count(999));
        EXPECT_EQ(1u, map.count(555));

        EXPECT_EQ(2u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));

        // The 999 resource will be sent to a grandparent compositor.
        break;
      }
      case 2: {
        EXPECT_EQ(2u, map.size());
        EXPECT_EQ(1u, map.count(999));
        EXPECT_EQ(1u, map.count(555));

        // 999 is in the parent, so not held by delegated renderer layer.
        EXPECT_EQ(1u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));

        // Receive 999 back from the grandparent.
        CompositorFrameAck ack;
        output_surface()->ReturnResource(map.find(999)->second, &ack);
        host_impl->OnSwapBuffersComplete(&ack);
        break;
      }
      case 3:
        // 999 should be released.
        EXPECT_EQ(1u, map.size());
        EXPECT_EQ(1u, map.count(555));

        EXPECT_EQ(1u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));
        break;
    }
  }

  virtual void AfterTest() OVERRIDE {}

  TransferableResource resource_in_grandparent;
};

SINGLE_AND_MULTI_THREAD_DELEGATING_RENDERER_TEST_F(
    LayerTreeHostDelegatedTestResourceSentToParent);

class LayerTreeHostDelegatedTestCommitWithoutTake
    : public LayerTreeHostDelegatedTestCaseSingleDelegatedLayer {
 public:
  virtual void BeginTest() OVERRIDE {
    // Prevent drawing with resources that are sent to the grandparent.
    layer_tree_host()->SetViewportSize(gfx::Size());
    PostSetNeedsCommitToMainThread();
  }

  virtual void DidCommit() OVERRIDE {
    scoped_ptr<DelegatedFrameData> frame;
    TransferableResourceArray resources;

    int next_source_frame_number = layer_tree_host()->source_frame_number();
    switch (next_source_frame_number) {
      case 1:
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTransferableResource(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        AddTransferableResource(frame.get(), 555);
        AddTextureQuad(frame.get(), 444);
        AddTransferableResource(frame.get(), 444);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 2:
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(0u, resources.size());

        // Stop using 999 and 444 in this frame and commit.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 3:
        // Don't take resources here, but set a new frame that uses 999 again.
        frame = CreateFrameData(gfx::Rect(0, 0, 1, 1), gfx::Rect(0, 0, 1, 1));
        AddTextureQuad(frame.get(), 999);
        AddTextureQuad(frame.get(), 555);
        delegated_->SetFrameData(frame.Pass());
        break;
      case 4:
        // 999 and 555 are in use, but 444 should be returned now.
        delegated_->TakeUnusedResourcesForChildCompositor(&resources);
        EXPECT_EQ(1u, resources.size());
        EXPECT_EQ(444u, resources[0].id);

        EndTest();
        break;
    }
  }

  virtual void DidActivateTreeOnThread(LayerTreeHostImpl* host_impl) OVERRIDE {
    if (host_impl->active_tree()->source_frame_number() < 1)
      return;

    LayerImpl* root_impl = host_impl->active_tree()->root_layer();
    FakeDelegatedRendererLayerImpl* delegated_impl =
        static_cast<FakeDelegatedRendererLayerImpl*>(root_impl->children()[0]);

    const ResourceProvider::ResourceIdMap& map =
        host_impl->resource_provider()->GetChildToParentMap(
            delegated_impl->ChildId());

    switch (host_impl->active_tree()->source_frame_number()) {
      case 1:
        EXPECT_EQ(3u, map.size());
        EXPECT_EQ(1u, map.count(999));
        EXPECT_EQ(1u, map.count(555));
        EXPECT_EQ(1u, map.count(444));

        EXPECT_EQ(3u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(444)->second));
        break;
      case 2:
        EXPECT_EQ(1u, map.size());
        EXPECT_EQ(1u, map.count(555));

        EXPECT_EQ(1u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));
        break;
      case 3:
        EXPECT_EQ(2u, map.size());
        EXPECT_EQ(1u, map.count(999));
        EXPECT_EQ(1u, map.count(555));

        EXPECT_EQ(2u, delegated_impl->Resources().size());
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(999)->second));
        EXPECT_EQ(1u, delegated_impl->Resources().count(map.find(555)->second));
    }
  }

  virtual void AfterTest() OVERRIDE {}
};

SINGLE_AND_MULTI_THREAD_TEST_F(LayerTreeHostDelegatedTestCommitWithoutTake);

}  // namespace
}  // namespace cc
