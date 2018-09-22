#include "render_scene.h"

#include "engine/array.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/fs/file_system.h"
#include "engine/geometry.h"
#include "engine/job_system.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/math_utils.h"
#include "engine/plugin_manager.h"
#include "engine/profiler.h"
#include "engine/reflection.h"
#include "engine/resource_manager.h"
#include "engine/serializer.h"
#include "engine/universe/universe.h"
#include "engine/viewport.h"
#include "lua_script/lua_script_system.h"
#include "renderer/culling_system.h"
#include "renderer/font_manager.h"
#include "renderer/material.h"
#include "renderer/material_manager.h"
#include "renderer/model.h"
#include "renderer/particle_system.h"
#include "renderer/pipeline.h"
#include "renderer/pose.h"
#include "renderer/renderer.h"
#include "renderer/terrain.h"
#include "renderer/texture.h"
#include <cfloat>
#include <cmath>
#include <algorithm>


namespace Lumix
{


enum class RenderSceneVersion : int
{

	LATEST
};


static const ComponentType MODEL_INSTANCE_TYPE = Reflection::getComponentType("model_instance");
static const ComponentType DECAL_TYPE = Reflection::getComponentType("decal");
static const ComponentType POINT_LIGHT_TYPE = Reflection::getComponentType("point_light");
static const ComponentType PARTICLE_EMITTER_TYPE = Reflection::getComponentType("particle_emitter");
static const ComponentType GLOBAL_LIGHT_TYPE = Reflection::getComponentType("global_light");
static const ComponentType CAMERA_TYPE = Reflection::getComponentType("camera");
static const ComponentType TERRAIN_TYPE = Reflection::getComponentType("terrain");
static const ComponentType BONE_ATTACHMENT_TYPE = Reflection::getComponentType("bone_attachment");
static const ComponentType ENVIRONMENT_PROBE_TYPE = Reflection::getComponentType("environment_probe");
static const ComponentType TEXT_MESH_TYPE = Reflection::getComponentType("text_mesh");


struct Decal : public DecalInfo
{
	EntityRef entity;
	Vec3 scale;
};


struct PointLight
{
	Vec3 m_diffuse_color;
	Vec3 m_specular_color;
	float m_diffuse_intensity;
	float m_specular_intensity;
	EntityRef m_entity;
	float m_fov;
	float m_attenuation_param;
	float m_range;
	bool m_cast_shadows;
};


struct GlobalLight
{
	Vec3 m_diffuse_color;
	float m_diffuse_intensity;
	float m_indirect_intensity;
	Vec3 m_fog_color;
	float m_fog_density;
	float m_fog_bottom;
	float m_fog_height;
	EntityRef m_entity;
	Vec4 m_cascades;
};


struct Camera
{
	EntityRef entity;
	float fov;
	float aspect;
	float near;
	float far;
	float ortho_size;
	float screen_width;
	float screen_height;
	bool is_ortho;
};


struct EnvironmentProbe
{
	enum Flags
	{
		REFLECTION = 1 << 0,
		OVERRIDE_GLOBAL_SIZE = 1 << 1,
		ENABLED = 1 << 2
	};

	Texture* texture;
	Texture* irradiance;
	Texture* radiance;
	float radius;
	u64 guid;
	FlagSet<Flags, u32> flags;
	u16 radiance_size = 128;
	u16 irradiance_size = 32;
	u16 reflection_size = 1024;
};


struct BoneAttachment
{
	EntityRef entity;
	EntityPtr parent_entity;
	int bone_index;
	LocalRigidTransform relative_transform;
};


struct TextMesh
{
	enum Flags : u32
	{
		CAMERA_ORIENTED = 1 << 0
	};
	
	TextMesh(IAllocator& allocator) : text("", allocator) {}
	~TextMesh() { setFontResource(nullptr); }

	void setFontResource(FontResource* res)
	{
		if (m_font_resource)
		{
			if (m_font)
			{
				m_font_resource->removeRef(*m_font);
				m_font = nullptr;
			}
			m_font_resource->getObserverCb().unbind<TextMesh, &TextMesh::onFontLoaded>(this);
			m_font_resource->getResourceManager().unload(*m_font_resource);
		}
		m_font_resource = res;
		if (res) res->onLoaded<TextMesh, &TextMesh::onFontLoaded>(this); 
	}

	void onFontLoaded(Resource::State, Resource::State new_state, Resource&)
	{
		if (new_state != Resource::State::READY)
		{
			m_font = nullptr;
		}
		else
		{
			m_font = m_font_resource->addRef(m_font_size);
		}
	}

	void setFontSize(int value)
	{
		m_font_size = value;
		if (m_font_resource && m_font_resource->isReady())
		{
			if(m_font) m_font_resource->removeRef(*m_font);
			m_font = m_font_resource->addRef(m_font_size);
		}
	}

	FontResource* getFontResource() const { return m_font_resource; }
	Font* getFont() const { return m_font; }
	int getFontSize() const { return m_font_size; }

	string text;
	u32 color = 0xff000000;
	FlagSet<Flags, u32> m_flags;

private:
	int m_font_size = 13;
	Font* m_font = nullptr;
	FontResource* m_font_resource = nullptr;

};


class RenderSceneImpl final : public RenderScene
{
private:
	struct CustomMeshCallback
	{
		CustomMeshCallback(CustomMeshCallback&& rhs)
			: m_entities(Move(rhs.m_entities))
			, m_scene(rhs.m_scene)
			, m_material(rhs.m_material)
		{
			rhs.m_material = nullptr;
		}

		CustomMeshCallback(const CustomMeshCallback&) = delete;

		CustomMeshCallback(RenderSceneImpl& scene, Material* material, IAllocator& allocator)
			: m_entities(allocator)
			, m_material(material)
			, m_scene(scene)
		{
			m_material->getObserverCb().bind<RenderSceneImpl, &RenderSceneImpl::materialStateChanged>(&scene);
		}

		~CustomMeshCallback()
		{
			if(m_material) {
				m_material->getObserverCb().unbind<RenderSceneImpl, &RenderSceneImpl::materialStateChanged>(&m_scene);
			}
		}

		RenderSceneImpl& m_scene;
		Material* m_material;
		Array<EntityRef> m_entities;
	};

	struct ModelLoadedCallback
	{
		ModelLoadedCallback(RenderSceneImpl& scene, Model* model)
			: m_scene(scene)
			, m_ref_count(0)
			, m_model(model)
		{
			m_model->getObserverCb().bind<RenderSceneImpl, &RenderSceneImpl::modelStateChanged>(&scene);
		}

		~ModelLoadedCallback()
		{
			m_model->getObserverCb().unbind<RenderSceneImpl, &RenderSceneImpl::modelStateChanged>(&m_scene);
		}

		Model* m_model;
		int m_ref_count;
		RenderSceneImpl& m_scene;
	};

public:
	RenderSceneImpl(Renderer& renderer,
		Engine& engine,
		Universe& universe,
		IAllocator& allocator);

	~RenderSceneImpl()
	{
		m_universe.entityTransformed().unbind<RenderSceneImpl, &RenderSceneImpl::onEntityMoved>(this);
		m_universe.entityDestroyed().unbind<RenderSceneImpl, &RenderSceneImpl::onEntityDestroyed>(this);
		CullingSystem::destroy(*m_culling_system);
	}


	void materialStateChanged(Resource::State old_state, Resource::State new_state, Resource& resource)
	{
		Material& material = (Material&)resource;
		const u64 layer_mask = new_state == Resource::State::READY ? material.getRenderLayerMask() : 0;
		auto iter = m_custom_mesh_callbacks.find(&material);
		const auto& entities = iter.value().m_entities;
		for(EntityRef e : entities) {
			ModelInstance& r = m_model_instances[e.index];
			ASSERT(hasCustomMeshes(r));
			for(int i = 0; i < r.mesh_count; ++i) {
				Mesh& m = r.meshes[i];
				if (m.material == &material) {
					m.layer_mask = layer_mask;
				}
			}
		}
	}


	void modelStateChanged(Resource::State old_state, Resource::State new_state, Resource& resource)
	{
		Model* model = static_cast<Model*>(&resource);
		if (new_state == Resource::State::READY)
		{
			modelLoaded(model);
		}
		else if (old_state == Resource::State::READY && new_state != Resource::State::READY)
		{
			modelUnloaded(model);
		}
	}


	void clear() override
	{
		auto& rm = m_engine.getResourceManager();
		auto* material_manager = static_cast<MaterialManager*>(rm.get(Material::TYPE));

		
		for (TextMesh* text_mesh : m_text_meshes)
		{
			LUMIX_DELETE(m_allocator, text_mesh);
		}
		m_text_meshes.clear();

		for (Decal& decal : m_decals)
		{
			if (decal.material) material_manager->unload(*decal.material);
		}
		m_decals.clear();

		m_cameras.clear();

		for (auto* terrain : m_terrains)
		{
			LUMIX_DELETE(m_allocator, terrain);
		}
		m_terrains.clear();

		for (auto* emitter : m_particle_emitters)
		{
			LUMIX_DELETE(m_allocator, emitter);
		}
		m_particle_emitters.clear();

		for (auto& i : m_model_instances)
		{
			if (i.entity != INVALID_ENTITY && i.model)
			{
				freeCustomMeshes(i, material_manager);
				i.model->getResourceManager().unload(*i.model);
				LUMIX_DELETE(m_allocator, i.pose);
			}
		}
		m_model_instances.clear();
		for(auto iter = m_model_entity_map.begin(), end = m_model_entity_map.end(); iter != end; ++iter) {
			Model* model = iter.key();
			model->getObserverCb().unbind<RenderSceneImpl, &RenderSceneImpl::modelStateChanged>(this);
		}
		m_model_entity_map.clear();

		m_culling_system->clear();

		for (auto& probe : m_environment_probes)
		{
			if (probe.texture) probe.texture->getResourceManager().unload(*probe.texture);
			if (probe.radiance) probe.radiance->getResourceManager().unload(*probe.radiance);
			if (probe.irradiance) probe.irradiance->getResourceManager().unload(*probe.irradiance);
		}
		m_environment_probes.clear();
		
		ASSERT(m_custom_mesh_callbacks.empty());
	}


	Universe& getUniverse() override { return m_universe; }


	IPlugin& getPlugin() const override { return m_renderer; }


	void getRay(EntityRef camera_entity,
		const Vec2& screen_pos,
		DVec3& origin,
		Vec3& dir) override
	{
		Camera& camera = m_cameras[camera_entity];
		origin = m_universe.getPosition(camera_entity);

		float width = camera.screen_width;
		float height = camera.screen_height;
		if (width <= 0 || height <= 0)
		{
			dir = m_universe.getRotation(camera_entity).rotate(Vec3(0, 0, 1));
			return;
		}

		float nx = 2 * (screen_pos.x / width) - 1;
		float ny = 2 * ((height - screen_pos.y) / height) - 1;

		const Matrix projection_matrix = getCameraProjection(camera_entity);
		const Transform view = m_universe.getTransform(camera_entity);

		if (camera.is_ortho) {
			const float ratio = camera.screen_height > 0 ? camera.screen_width / camera.screen_height : 1;
			origin += view.rot * Vec3(1, 0, 0) * nx * camera.ortho_size * ratio
				+ view.rot * Vec3(0, 1, 0) * ny * camera.ortho_size;
		}

		Matrix inv_projection = projection_matrix;
		inv_projection.inverse();

		Vec4 p0 = inv_projection * Vec4(nx, ny, -1, 1);
		Vec4 p1 = inv_projection * Vec4(nx, ny, 1, 1);
		p0 *= 1 / p0.w;
		p1 *= 1 / p1.w;
		dir = (p1 - p0).xyz();
		dir.normalize();
		dir = view.rot * dir;
	}

	
	EntityPtr getActiveCamera() const override
	{
		return m_active_camera;
	}


	Viewport getCameraViewport(EntityRef entity) const override
	{
		Viewport vp;
		const Camera& cam = m_cameras[entity];
		vp.far = cam.far;
		vp.near = cam.near;
		vp.is_ortho = cam.is_ortho;
		vp.h = (int)cam.screen_height;
		vp.w = (int)cam.screen_width;
		if(vp.is_ortho) {
			vp.ortho_size = cam.ortho_size;
		}
		else {
			vp.fov = cam.fov;
		}
		vp.pos = m_universe.getPosition(entity);
		vp.rot = m_universe.getRotation(entity);
		return vp;
	}


	float getCameraLODMultiplier(float fov, bool is_ortho) const override
	{
		if (is_ortho) return 1;

		const float lod_multiplier = fov / Math::degreesToRadians(60);
		return lod_multiplier  * lod_multiplier;
	}


	float getCameraLODMultiplier(EntityRef entity) const override
	{
		const Camera& camera = m_cameras[entity];
		return getCameraLODMultiplier(camera.fov, camera.is_ortho);
	}


	ShiftedFrustum getCameraFrustum(EntityRef entity) const override
	{
		ShiftedFrustum ret;
		const Camera& camera = m_cameras[entity];
		const Transform tr = m_universe.getTransform(entity);
		float ratio = camera.screen_height > 0 ? camera.screen_width / camera.screen_height : 1;
		if (camera.is_ortho) {
			ret.computeOrtho(tr.pos,
				tr.rot * Vec3(0, 0, 1),
				tr.rot * Vec3(0, 1, 0),
				camera.ortho_size * ratio,
				camera.ortho_size,
				camera.near,
				camera.far);
			return ret;
		}

		ret.computePerspective(tr.pos,
			tr.rot * Vec3(0, 0, -1),
			tr.rot * Vec3(0, 1, 0),
			camera.fov,
			ratio,
			camera.near,
			camera.far);
		return ret;
	}


	ShiftedFrustum getCameraFrustum(EntityRef entity, const Vec2& viewport_min_px, const Vec2& viewport_max_px) const override
	{
		ShiftedFrustum ret;
		const Camera& camera = m_cameras[entity];
		const Transform tr = m_universe.getTransform(entity);
		float ratio = camera.screen_height > 0 ? camera.screen_width / camera.screen_height : 1;
		Vec2 viewport_min = { viewport_min_px.x / camera.screen_width * 2 - 1, (1 - viewport_max_px.y / camera.screen_height) * 2 - 1 };
		Vec2 viewport_max = { viewport_max_px.x / camera.screen_width * 2 - 1, (1 - viewport_min_px.y / camera.screen_height) * 2 - 1 };
		if (camera.is_ortho) {
			ret.computeOrtho(tr.pos,
				tr.rot * Vec3(0, 0, 1),
				tr.rot * Vec3(0, 1, 0),
				camera.ortho_size * ratio,
				camera.ortho_size,
				camera.near,
				camera.far,
				viewport_min,
				viewport_max);
			return ret;
		}

		ret.computePerspective(tr.pos,
			tr.rot * Vec3(0, 0, -1),
			tr.rot * Vec3(0, 1, 0),
			camera.fov,
			ratio,
			camera.near,
			camera.far,
			viewport_min,
			viewport_max);
		return ret;
	}


	void updateBoneAttachment(const BoneAttachment& bone_attachment)
	{
		if (!bone_attachment.parent_entity.isValid()) return;
		const EntityPtr model_instance_ptr = bone_attachment.parent_entity;
		if (!model_instance_ptr.isValid()) return;

		const EntityRef model_instance = (EntityRef)model_instance_ptr;
		if (!m_universe.hasComponent(model_instance, MODEL_INSTANCE_TYPE)) return;
		const Pose* parent_pose = lockPose(model_instance);
		if (!parent_pose) return;

		Transform parent_entity_transform = m_universe.getTransform((EntityRef)bone_attachment.parent_entity);
		int idx = bone_attachment.bone_index;
		if (idx < 0 || idx > parent_pose->count) {
			unlockPose(model_instance, false);
			return;
		}
		float original_scale = m_universe.getScale(bone_attachment.entity);
		const LocalRigidTransform bone_transform = {parent_pose->positions[idx], parent_pose->rotations[idx] };
		const LocalRigidTransform relative_transform = { bone_attachment.relative_transform.pos, bone_attachment.relative_transform.rot };
		Transform result = parent_entity_transform * bone_transform * relative_transform;
		result.scale = original_scale;
		m_universe.setTransform(bone_attachment.entity, result);
		unlockPose(model_instance, false);
	}


	EntityPtr getBoneAttachmentParent(EntityRef entity) override
	{
		return m_bone_attachments[entity].parent_entity;
	}


	void updateRelativeMatrix(BoneAttachment& attachment)
	{
		if (!attachment.parent_entity.isValid()) return;
		if (attachment.bone_index < 0) return;
		const EntityPtr model_instance_ptr = attachment.parent_entity;
		if (!model_instance_ptr.isValid()) return;
		const EntityRef model_instance = (EntityRef)model_instance_ptr;
		if (!m_universe.hasComponent(model_instance, MODEL_INSTANCE_TYPE)) return;
		const Pose* pose = lockPose(model_instance);
		if (!pose) return;

		ASSERT(pose->is_absolute);
		if (attachment.bone_index >= pose->count) {
			unlockPose(model_instance, false);
			return;
		}
		const LocalRigidTransform bone_transform = {pose->positions[attachment.bone_index], pose->rotations[attachment.bone_index]};

		const EntityRef parent = (EntityRef)attachment.parent_entity;
		Transform inv_parent_transform = m_universe.getTransform(parent) * bone_transform;
		inv_parent_transform = inv_parent_transform.inverted();
		const Transform child_transform = m_universe.getTransform(attachment.entity);
		const Transform res = inv_parent_transform * child_transform;
		attachment.relative_transform = {res.pos.toFloat(), res.rot};
		unlockPose(model_instance, false);
	}


	Vec3 getBoneAttachmentPosition(EntityRef entity) override
	{
		return m_bone_attachments[entity].relative_transform.pos;
		
	}


	void setBoneAttachmentPosition(EntityRef entity, const Vec3& pos) override
	{
		BoneAttachment& attachment = m_bone_attachments[entity];
		attachment.relative_transform.pos = pos;
		m_is_updating_attachments = true;
		updateBoneAttachment(attachment);
		m_is_updating_attachments = false;
	}


	Vec3 getBoneAttachmentRotation(EntityRef entity) override
	{
		return m_bone_attachments[entity].relative_transform.rot.toEuler();
	}


	void setBoneAttachmentRotation(EntityRef entity, const Vec3& rot) override
	{
		BoneAttachment& attachment = m_bone_attachments[entity];
		Vec3 euler = rot;
		euler.x = Math::clamp(euler.x, -Math::PI * 0.5f, Math::PI * 0.5f);
		attachment.relative_transform.rot.fromEuler(euler);
		m_is_updating_attachments = true;
		updateBoneAttachment(attachment);
		m_is_updating_attachments = false;
	}


	void setBoneAttachmentRotationQuat(EntityRef entity, const Quat& rot) override
	{
		BoneAttachment& attachment = m_bone_attachments[entity];
		attachment.relative_transform.rot = rot;
		m_is_updating_attachments = true;
		updateBoneAttachment(attachment);
		m_is_updating_attachments = false;
	}


	int getBoneAttachmentBone(EntityRef entity) override
	{
		return m_bone_attachments[entity].bone_index;
	}


	void setBoneAttachmentBone(EntityRef entity, int value) override
	{
		BoneAttachment& ba = m_bone_attachments[entity];
		ba.bone_index = value;
		updateRelativeMatrix(ba);
	}


	void setBoneAttachmentParent(EntityRef entity, EntityPtr parent) override
	{
		BoneAttachment& ba = m_bone_attachments[entity];
		ba.parent_entity = parent;
		if (parent.isValid() && parent.index < m_model_instances.size())
		{
			ModelInstance& mi = m_model_instances[parent.index];
			mi.flags.set(ModelInstance::IS_BONE_ATTACHMENT_PARENT);
		}
		updateRelativeMatrix(ba);
	}

	void startGame() override
	{
		m_is_game_running = true;
	}


	void stopGame() override
	{
		m_is_game_running = false;
	}


	void update(float dt, bool paused) override
	{
		PROFILE_FUNCTION();

		m_time += dt;
		for (int i = m_debug_triangles.size() - 1; i >= 0; --i)
		{
			float life = m_debug_triangles[i].life;
			if (life < 0)
			{
				m_debug_triangles.eraseFast(i);
			}
			else
			{
				life -= dt;
				m_debug_triangles[i].life = life;
			}
		}

		for(int i = m_debug_lines.size() - 1; i >= 0; --i)
		{
			float life = m_debug_lines[i].life;
			if(life < 0)
			{
				m_debug_lines.eraseFast(i);
			}
			else
			{
				life -= dt;
				m_debug_lines[i].life = life;
			}
		}


		for (int i = m_debug_points.size() - 1; i >= 0; --i)
		{
			float life = m_debug_points[i].life;
			if (life < 0)
			{
				m_debug_points.eraseFast(i);
			}
			else
			{
				life -= dt;
				m_debug_points[i].life = life;
			}
		}

		if (m_is_game_running && !paused)
		{
			for (auto* emitter : m_particle_emitters)
			{
				emitter->update(dt);
			}
		}
	}


	void serializeModelInstance(ISerializer& serialize, EntityRef entity)
	{
		ModelInstance& r = m_model_instances[entity.index];
		ASSERT(r.entity != INVALID_ENTITY);

		serialize.write("source", r.model ? r.model->getPath().c_str() : "");
		serialize.write("flags", u8(r.flags.base & ModelInstance::PERSISTENT_FLAGS));
		bool has_changed_materials = r.model && r.model->isReady() && r.meshes != &r.model->getMesh(0);
		serialize.write("custom_materials", has_changed_materials ? r.mesh_count : 0);
		if (has_changed_materials)
		{
			for (int i = 0; i < r.mesh_count; ++i)
			{
				serialize.write("", r.meshes[i].material->getPath().c_str());
			}
		}
	}


	static bool hasCustomMeshes(ModelInstance& r)
	{
		return r.flags.isSet(ModelInstance::CUSTOM_MESHES);
	}


	void deserializeModelInstance(IDeserializer& serializer, EntityRef entity, int scene_version)
	{
		while (entity.index >= m_model_instances.size())
		{
			auto& r = m_model_instances.emplace();
			r.entity = INVALID_ENTITY;
			r.pose = nullptr;
			r.model = nullptr;
			r.meshes = nullptr;
			r.mesh_count = 0;
		}
		auto& r = m_model_instances[entity.index];
		r.entity = entity;
		r.model = nullptr;
		r.pose = nullptr;
		r.flags.clear();
		r.flags.set(ModelInstance::ENABLED);
		r.meshes = nullptr;
		r.mesh_count = 0;

		char path[MAX_PATH_LENGTH];
		serializer.read(path, lengthOf(path));
		serializer.read(&r.flags.base);
		r.flags.base &= ModelInstance::PERSISTENT_FLAGS;

		if (path[0] != 0)
		{
			auto* model = m_engine.getResourceManager().load<Model>(Path(path));
			setModel(entity, model);
		}

		int material_count;
		serializer.read(&material_count);
		if (material_count > 0)
		{
			allocateCustomMeshes(r, material_count);
			for (int j = 0; j < material_count; ++j)
			{
				char path[MAX_PATH_LENGTH];
				serializer.read(path, lengthOf(path));
				setModelInstanceMaterial(entity, j, Path(path));
			}
		}

		m_universe.onComponentCreated(entity, MODEL_INSTANCE_TYPE, this);
	}


	void serializeGlobalLight(ISerializer& serializer, EntityRef entity)
	{
		GlobalLight& light = m_global_lights[entity];
		serializer.write("cascades", light.m_cascades);
		serializer.write("diffuse_color", light.m_diffuse_color);
		serializer.write("diffuse_intensity", light.m_diffuse_intensity);
		serializer.write("indirect_intensity", light.m_indirect_intensity);
		serializer.write("fog_bottom", light.m_fog_bottom);
		serializer.write("fog_color", light.m_fog_color);
		serializer.write("fog_density", light.m_fog_density);
		serializer.write("fog_height", light.m_fog_height);
	}


	void deserializeGlobalLight(IDeserializer& serializer, EntityRef entity, int scene_version)
	{
		GlobalLight light;
		light.m_entity = entity;
		serializer.read(&light.m_cascades);
		serializer.read(&light.m_diffuse_color);
		serializer.read(&light.m_diffuse_intensity);
		serializer.read(&light.m_indirect_intensity);
		serializer.read(&light.m_fog_bottom);
		serializer.read(&light.m_fog_color);
		serializer.read(&light.m_fog_density);
		serializer.read(&light.m_fog_height);
		m_global_lights.insert(entity, light);
		m_universe.onComponentCreated(light.m_entity, GLOBAL_LIGHT_TYPE, this);
		m_active_global_light_entity = entity;
	}
	
	
	void serializePointLight(ISerializer& serializer, EntityRef entity)
	{
		PointLight& light = m_point_lights[m_point_lights_map[entity]];
		serializer.write("attenuation", light.m_attenuation_param);
		serializer.write("cast_shadow", light.m_cast_shadows);
		serializer.write("diffuse_color", light.m_diffuse_color);
		serializer.write("diffuse_intensity", light.m_diffuse_intensity);
		serializer.write("fov", light.m_fov);
		serializer.write("range", light.m_range);
		serializer.write("specular_color", light.m_specular_color);
		serializer.write("specular_intensity", light.m_specular_intensity);
	}


	void deserializePointLight(IDeserializer& serializer, EntityRef entity, int scene_version)
	{
		PointLight& light = m_point_lights.emplace();
		light.m_entity = entity;
		serializer.read(&light.m_attenuation_param);
		serializer.read(&light.m_cast_shadows);
		serializer.read(&light.m_diffuse_color);
		serializer.read(&light.m_diffuse_intensity);
		serializer.read(&light.m_fov);
		serializer.read(&light.m_range);
		serializer.read(&light.m_specular_color);
		serializer.read(&light.m_specular_intensity);
		m_point_lights_map.insert(light.m_entity, m_point_lights.size() - 1);

		m_universe.onComponentCreated(light.m_entity, POINT_LIGHT_TYPE, this);
	}


	void serializeDecal(ISerializer& serializer, EntityRef entity)
	{
		const Decal& decal = m_decals[entity];
		serializer.write("scale", decal.scale);
		serializer.write("material", decal.material ? decal.material->getPath().c_str() : "");
	}


	void deserializeDecal(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		ResourceManagerHub& manager = m_engine.getResourceManager();
		Decal& decal = m_decals.insert(entity);
		char tmp[MAX_PATH_LENGTH];
		decal.entity = entity;
		serializer.read(&decal.scale);
		serializer.read(tmp, lengthOf(tmp));
		decal.material = tmp[0] == '\0' ? nullptr : manager.load<Material>(Path(tmp));
		updateDecalInfo(decal);
		m_universe.onComponentCreated(decal.entity, DECAL_TYPE, this);
	}


	void serializeTextMesh(ISerializer& serializer, EntityRef entity)
	{
		TextMesh& text = *m_text_meshes.get(entity);
		serializer.write("font", text.getFontResource() ? text.getFontResource()->getPath().c_str() : "");
		serializer.write("color", text.color);
		serializer.write("font_size", text.getFontSize());
		serializer.write("text", text.text.c_str());
	}


	void setTextMeshText(EntityRef entity, const char* text) override
	{
		m_text_meshes.get(entity)->text = text;
	}


	const char* getTextMeshText(EntityRef entity) override
	{
		return m_text_meshes.get(entity)->text.c_str();
	}


	bool isTextMeshCameraOriented(EntityRef entity) override
	{
		TextMesh& text = *m_text_meshes.get(entity);
		return text.m_flags.isSet(TextMesh::CAMERA_ORIENTED);
	}


	void setTextMeshCameraOriented(EntityRef entity, bool is_oriented) override
	{
		TextMesh& text = *m_text_meshes.get(entity);
		text.m_flags.set(TextMesh::CAMERA_ORIENTED, is_oriented);
	}


	void setTextMeshFontSize(EntityRef entity, int value) override
	{
		TextMesh& text = *m_text_meshes.get(entity);
		text.setFontSize(value);
	}


	int getTextMeshFontSize(EntityRef entity) override
	{
		return m_text_meshes.get(entity)->getFontSize();
	}


	static Vec4 ABGRu32ToRGBAVec4(u32 value)
	{
		float inv = 1 / 255.0f;
		return {
			((value >> 0) & 0xFF) * inv,
			((value >> 8) & 0xFF) * inv,
			((value >> 16) & 0xFF) * inv,
			((value >> 24) & 0xFF) * inv,
		};
	}


	static u32 RGBAVec4ToABGRu32(const Vec4& value)
	{
		u8 r = u8(value.x * 255 + 0.5f);
		u8 g = u8(value.y * 255 + 0.5f);
		u8 b = u8(value.z * 255 + 0.5f);
		u8 a = u8(value.w * 255 + 0.5f);
		return (a << 24) + (b << 16) + (g << 8) + r;
	}


	Vec4 getTextMeshColorRGBA(EntityRef entity) override
	{
		return ABGRu32ToRGBAVec4(m_text_meshes.get(entity)->color);
	}


	void setTextMeshColorRGBA(EntityRef entity, const Vec4& color) override
	{
		m_text_meshes.get(entity)->color = RGBAVec4ToABGRu32(color);
	}


	Path getTextMeshFontPath(EntityRef entity) override
	{
		TextMesh& text = *m_text_meshes.get(entity);
		return text.getFontResource() == nullptr ? Path() : text.getFontResource()->getPath();
	}


	void getTextMeshesVertices(Array<TextMeshVertex>& vertices, EntityRef camera) override
	{
				// TODO
		ASSERT(false);
		/*
		Matrix camera_mtx = m_universe.getMatrix(camera);
		Vec3 cam_right = camera_mtx.getXVector();
		Vec3 cam_up = -camera_mtx.getYVector();
		for (int j = 0, nj = m_text_meshes.size(); j < nj; ++j)
		{
			TextMesh& text = *m_text_meshes.at(j);
			Font* font = text.getFont();
			if (!font) font = m_renderer.getFontManager().getDefaultFont();
			EntityRef entity = m_text_meshes.getKey(j);
			const char* str = text.text.c_str();
			Vec3 base = m_universe.getPosition(entity);
			Quat rot = m_universe.getRotation(entity);
			float scale = m_universe.getScale(entity);
			Vec3 right = rot.rotate({ 1, 0, 0 }) * scale;
			Vec3 up = rot.rotate({ 0, -1, 0 }) * scale;
			if (text.m_flags.isSet(TextMesh::CAMERA_ORIENTED))
			{
				right = cam_right * scale;
				up = cam_up * scale;
			}
			u32 color = text.color;
			Vec2 text_size = font->CalcTextSizeA((float)text.getFontSize(), FLT_MAX, 0, str);
			base += right * text_size.x * -0.5f;
			base += up * text_size.y * -0.5f;
			for (int i = 0, n = text.text.length(); i < n; ++i)
			{
				const Font::Glyph* glyph = font->FindGlyph(str[i]);
				if (!glyph) continue;

				Vec3 x0y0 = base + right * glyph->X0 + up * glyph->Y0;
				Vec3 x1y0 = base + right * glyph->X1 + up * glyph->Y0;
				Vec3 x1y1 = base + right * glyph->X1 + up * glyph->Y1;
				Vec3 x0y1 = base + right * glyph->X0 + up * glyph->Y1;

				vertices.push({ x0y0, color, { glyph->U0, glyph->V0 } });
				vertices.push({ x1y0, color, { glyph->U1, glyph->V0 } });
				vertices.push({ x1y1, color, { glyph->U1, glyph->V1 } });

				vertices.push({ x0y0, color, { glyph->U0, glyph->V0 } });
				vertices.push({ x1y1, color, { glyph->U1, glyph->V1 } });
				vertices.push({ x0y1, color, { glyph->U0, glyph->V1 } });
				
				base += right * glyph->XAdvance;
			}

		}*/
	}


	void setTextMeshFontPath(EntityRef entity, const Path& path) override
	{
		TextMesh& text = *m_text_meshes.get(entity);
		ResourceManagerHub& manager = m_renderer.getEngine().getResourceManager();
		FontResource* res = path.isValid() ? manager.load<FontResource>(path) : nullptr;
		text.setFontResource(res);
	}

	
	void deserializeTextMesh(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		TextMesh& text = *LUMIX_NEW(m_allocator, TextMesh)(m_allocator);
		m_text_meshes.insert(entity, &text);

		char tmp[MAX_PATH_LENGTH];
		serializer.read(tmp, lengthOf(tmp));
		serializer.read(&text.color);
		int font_size;
		serializer.read(&font_size);
		text.setFontSize(font_size);
		serializer.read(&text.text);
		ResourceManagerHub& manager = m_renderer.getEngine().getResourceManager();
		FontResource* res = tmp[0] ? manager.load<FontResource>(Path(tmp)) : nullptr;
		text.setFontResource(res);
		m_universe.onComponentCreated(entity, TEXT_MESH_TYPE, this);
	}


	void serializeCamera(ISerializer& serialize, EntityRef entity)
	{
		Camera& camera = m_cameras[entity];
		serialize.write("far", camera.far);
		serialize.write("fov", camera.fov);
		serialize.write("is_ortho", camera.is_ortho);
		serialize.write("ortho_size", camera.ortho_size);
		serialize.write("near", camera.near);
	}


	void deserializeCamera(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		Camera camera;
		camera.entity = entity;
		serializer.read(&camera.far);
		serializer.read(&camera.fov);
		serializer.read(&camera.is_ortho);
		serializer.read(&camera.ortho_size);
		serializer.read(&camera.near);
		m_cameras.insert(camera.entity, camera);
		m_universe.onComponentCreated(camera.entity, CAMERA_TYPE, this);
	}


	void serializeBoneAttachment(ISerializer& serializer, EntityRef entity) 
	{
		BoneAttachment& attachment = m_bone_attachments[entity];
		serializer.write("bone_index", attachment.bone_index);
		serializer.write("parent", attachment.parent_entity);
		serializer.write("relative_transform", attachment.relative_transform);
	}


	void deserializeBoneAttachment(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		BoneAttachment& bone_attachment = m_bone_attachments.emplace(entity);
		bone_attachment.entity = entity;
		serializer.read(&bone_attachment.bone_index);
		serializer.read(&bone_attachment.parent_entity);
		serializer.read(&bone_attachment.relative_transform);
		m_universe.onComponentCreated(bone_attachment.entity, BONE_ATTACHMENT_TYPE, this);
		EntityPtr parent_entity = bone_attachment.parent_entity;
		if (parent_entity.isValid() && parent_entity.index < m_model_instances.size())
		{
			ModelInstance& mi = m_model_instances[parent_entity.index];
			mi.flags.set(ModelInstance::IS_BONE_ATTACHMENT_PARENT);
		}
	}


	void serializeTerrain(ISerializer& serializer, EntityRef entity)
	{
		Terrain* terrain = m_terrains[entity];
		serializer.write("layer_mask", terrain->m_layer_mask);
		serializer.write("scale", terrain->m_scale);
		serializer.write("material", terrain->m_material ? terrain->m_material->getPath().c_str() : "");
		serializer.write("grass_count", terrain->m_grass_types.size());
		for (Terrain::GrassType& type : terrain->m_grass_types)
		{
			serializer.write("density", type.m_density);
			serializer.write("distance", type.m_distance);
			serializer.write("rotation_mode", (int)type.m_rotation_mode);
			serializer.write("model", type.m_grass_model ? type.m_grass_model->getPath().c_str() : "");
		}
	}

	void deserializeTerrain(IDeserializer& serializer, EntityRef entity, int version)
	{
		Terrain* terrain = LUMIX_NEW(m_allocator, Terrain)(m_renderer, entity, *this, m_allocator);
		m_terrains.insert(entity, terrain);
		terrain->m_entity = entity;
		serializer.read(&terrain->m_layer_mask);
		serializer.read(&terrain->m_scale);
		char tmp[MAX_PATH_LENGTH];
		serializer.read(tmp, lengthOf(tmp));
		auto* material = tmp[0] ? m_engine.getResourceManager().load<Material>(Path(tmp)) : nullptr;
		terrain->setMaterial(material);

		int count;
		serializer.read(&count);
		for(int i = 0; i < count; ++i)
		{
			Terrain::GrassType type(*terrain);
			serializer.read(&type.m_density);
			serializer.read(&type.m_distance);
			serializer.read((int*)&type.m_rotation_mode);
			type.m_idx = i;
			serializer.read(tmp, lengthOf(tmp));
			terrain->m_grass_types.push(type);
			terrain->setGrassTypePath(terrain->m_grass_types.size() - 1, Path(tmp));
		}

		m_universe.onComponentCreated(entity, TERRAIN_TYPE, this);
	}

	void serializeEnvironmentProbe(ISerializer& serializer, EntityRef entity) 
	{
		EnvironmentProbe& probe = m_environment_probes[entity];
		serializer.write("guid", probe.guid);
		serializer.write("flags", probe.flags.base);
		serializer.write("radius", probe.radius);
		serializer.write("radiance_size", probe.radiance_size);
		serializer.write("irradiance_size", probe.irradiance_size);
		serializer.write("reflection_size", probe.reflection_size);
	}


	int getVersion() const override { return (int)RenderSceneVersion::LATEST; }


	void deserializeEnvironmentProbe(IDeserializer& serializer, EntityRef entity, int scene_version)
	{
		ResourceManagerHub& manager = m_engine.getResourceManager();
		StaticString<MAX_PATH_LENGTH> probe_dir("universes/", m_universe.getName(), "/probes/");
		EnvironmentProbe& probe = m_environment_probes.insert(entity);
		serializer.read(&probe.guid);
		serializer.read(&probe.flags.base);
		serializer.read(&probe.radius);
		serializer.read(&probe.radiance_size);
		serializer.read(&probe.irradiance_size);
		serializer.read(&probe.reflection_size);

		StaticString<MAX_PATH_LENGTH> path_str(probe_dir, probe.guid, ".dds");
		
		probe.texture = nullptr;
		if (probe.flags.isSet(EnvironmentProbe::REFLECTION)) {
			probe.texture = manager.load<Texture>(Path(path_str));
			probe.texture->setFlag(Texture::Flags::SRGB, true);
		}
		
		StaticString<MAX_PATH_LENGTH> irr_path_str(probe_dir, probe.guid, "_irradiance.dds");
		probe.irradiance = manager.load<Texture>(Path(irr_path_str));
		probe.irradiance->setFlag(Texture::Flags::SRGB, true);
		// TODO
		//probe.irradiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
		//probe.irradiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);
		StaticString<MAX_PATH_LENGTH> r_path_str(probe_dir, probe.guid, "_radiance.dds");
		probe.radiance = manager.load<Texture>(Path(r_path_str));
		probe.radiance->setFlag(Texture::Flags::SRGB, true);
		// TODO
		//probe.radiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
		//probe.radiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);

		m_universe.onComponentCreated(entity, ENVIRONMENT_PROBE_TYPE, this);
	}


	void serializeParticleEmitter(ISerializer& serializer, EntityRef entity)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		const ParticleEmitterResource* res = emitter->getResource();
		serializer.write("resource", res ? res->getPath().c_str() : "");
	}


	void deserializeParticleEmitter(IDeserializer& serializer, EntityRef entity, int scene_version)
	{
		ParticleEmitter* emitter = LUMIX_NEW(m_allocator, ParticleEmitter)(entity, m_allocator);
		emitter->m_entity = entity;

		char tmp[MAX_PATH_LENGTH];
		serializer.read(tmp, lengthOf(tmp));
		ResourceManagerHub& manager = m_engine.getResourceManager();
		ParticleEmitterResource* res = manager.load<ParticleEmitterResource>(Path(tmp));
		emitter->setResource(res);

		m_particle_emitters.insert(entity, emitter);
		m_universe.onComponentCreated(entity, PARTICLE_EMITTER_TYPE, this);
	}


	void serializeBoneAttachments(OutputBlob& serializer)
	{
		serializer.write((i32)m_bone_attachments.size());
		for (auto& attachment : m_bone_attachments)
		{
			serializer.write(attachment.bone_index);
			serializer.write(attachment.entity);
			serializer.write(attachment.parent_entity);
			serializer.write(attachment.relative_transform);
		}
	}

	void serializeCameras(OutputBlob& serializer)
	{
		serializer.write((i32)m_cameras.size());
		for (auto& camera : m_cameras)
		{
			serializer.write(camera.entity);
			serializer.write(camera.far);
			serializer.write(camera.fov);
			serializer.write(camera.is_ortho);
			serializer.write(camera.ortho_size);
			serializer.write(camera.near);
		}
	}

	void serializeLights(OutputBlob& serializer)
	{
		serializer.write((i32)m_point_lights.size());
		for (int i = 0, c = m_point_lights.size(); i < c; ++i)
		{
			serializer.write(m_point_lights[i]);
		}

		serializer.write((i32)m_global_lights.size());
		for (const GlobalLight& light : m_global_lights)
		{
			serializer.write(light);
		}
		serializer.write(m_active_global_light_entity);
	}

	void serializeModelInstances(OutputBlob& serializer)
	{
		serializer.write((i32)m_model_instances.size());
		for (auto& r : m_model_instances)
		{
			serializer.write(r.entity);
			serializer.write(u8(r.flags.base & ModelInstance::PERSISTENT_FLAGS));
			if(r.entity != INVALID_ENTITY)
			{
				serializer.write(r.model ? r.model->getPath().getHash() : 0);
				bool has_changed_materials = r.model && r.model->isReady() && r.meshes != &r.model->getMesh(0);
				serializer.write(has_changed_materials ? r.mesh_count : 0);
				if (has_changed_materials)
				{
					for (int i = 0; i < r.mesh_count; ++i)
					{
						serializer.writeString(r.meshes[i].material->getPath().c_str());
					}
				}
			}
			
		}
	}

	void serializeTerrains(OutputBlob& serializer)
	{
		serializer.write((i32)m_terrains.size());
		for (auto* terrain : m_terrains)
		{
			terrain->serialize(serializer);
		}
	}

	void serializeTextMeshes(OutputBlob& serializer)
	{
		serializer.write(m_text_meshes.size());
		for (int i = 0, n = m_text_meshes.size(); i < n; ++i)
		{
			TextMesh& text = *m_text_meshes.at(i);
			EntityRef e = m_text_meshes.getKey(i);
			serializer.write(e);
			serializer.writeString(text.getFontResource() ? text.getFontResource()->getPath().c_str() : "");
			serializer.write(text.color);
			serializer.write(text.getFontSize());
			serializer.write(text.text);
		}
	}

	void deserializeTextMeshes(InputBlob& serializer)
	{
		int count;
		serializer.read(count);
		ResourceManagerHub& manager = m_renderer.getEngine().getResourceManager();
		
		for (int i = 0; i < count; ++i) {
			EntityRef e;
			serializer.read(e);
			TextMesh& text = *LUMIX_NEW(m_allocator, TextMesh)(m_allocator);
			m_text_meshes.insert(e, &text);
			char tmp[MAX_PATH_LENGTH];
			serializer.readString(tmp, lengthOf(tmp));
			serializer.read(text.color);
			int font_size;
			serializer.read(font_size);
			text.setFontSize(font_size);
			serializer.read(text.text);
			FontResource* res = tmp[0] ? manager.load<FontResource>(Path(tmp)) : nullptr;
			text.setFontResource(res);
			m_universe.onComponentCreated(e, TEXT_MESH_TYPE, this);
		}
	}


	void deserializeDecals(InputBlob& serializer)
	{
		ResourceManagerHub& manager = m_engine.getResourceManager();
		int count;
		serializer.read(count);
		m_decals.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			char tmp[MAX_PATH_LENGTH];
			Decal decal;
			serializer.read(decal.entity);
			serializer.read(decal.scale);
			serializer.readString(tmp, lengthOf(tmp));
			decal.material = tmp[0] == '\0' ? nullptr : manager.load<Material>(Path(tmp));
			updateDecalInfo(decal);
			m_decals.insert(decal.entity, decal);
			m_universe.onComponentCreated(decal.entity, DECAL_TYPE, this);
		}
	}


	void serializeDecals(OutputBlob& serializer)
	{
		serializer.write(m_decals.size());
		for (auto& decal : m_decals)
		{
			serializer.write(decal.entity);
			serializer.write(decal.scale);
			serializer.writeString(decal.material ? decal.material->getPath().c_str() : "");
		}
	}


	void serializeEnvironmentProbes(OutputBlob& serializer)
	{
		i32 count = m_environment_probes.size();
		serializer.write(count);
		for (int i = 0; i < count; ++i)
		{
			EntityRef entity = m_environment_probes.getKey(i);
			serializer.write(entity);
			const EnvironmentProbe& probe = m_environment_probes.at(i);
			serializer.write(probe.radius);
			serializer.write(probe.guid);
			serializer.write(probe.flags.base);
			serializer.write(probe.radiance_size);
			serializer.write(probe.irradiance_size);
			serializer.write(probe.reflection_size);
		}
	}


	void deserializeEnvironmentProbes(InputBlob& serializer)
	{
		i32 count;
		serializer.read(count);
		m_environment_probes.reserve(count);
		ResourceManagerHub& manager = m_engine.getResourceManager();
		StaticString<MAX_PATH_LENGTH> probe_dir("universes/", m_universe.getName(), "/probes/");
		for (int i = 0; i < count; ++i)
		{
			EntityRef entity;
			serializer.read(entity);
			EnvironmentProbe& probe = m_environment_probes.insert(entity);
			serializer.read(probe.guid);
			serializer.read(probe.flags.base);
			serializer.read(probe.radiance_size);
			serializer.read(probe.irradiance_size);
			serializer.read(probe.reflection_size);
			probe.texture = nullptr;
			if (probe.flags.isSet(EnvironmentProbe::REFLECTION))
			{
				StaticString<MAX_PATH_LENGTH> path_str(probe_dir, probe.guid, ".dds");
				probe.texture = manager.load<Texture>(Path(path_str));
				probe.texture->setFlag(Texture::Flags::SRGB, true);
			}
			StaticString<MAX_PATH_LENGTH> irr_path_str(probe_dir, probe.guid, "_irradiance.dds");
			probe.irradiance = manager.load<Texture>(Path(irr_path_str));
			probe.irradiance->setFlag(Texture::Flags::SRGB, true);
			// TODO
			//probe.irradiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
			//probe.irradiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);
			StaticString<MAX_PATH_LENGTH> r_path_str(probe_dir, probe.guid, "_radiance.dds");
			probe.radiance = manager.load<Texture>(Path(r_path_str));
			probe.radiance->setFlag(Texture::Flags::SRGB, true);
			// TODO//probe.radiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
			//probe.radiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);

			m_universe.onComponentCreated(entity, ENVIRONMENT_PROBE_TYPE, this);
		}
	}


	void deserializeBoneAttachments(InputBlob& serializer)
	{
		i32 count;
		serializer.read(count);
		m_bone_attachments.clear();
		m_bone_attachments.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			BoneAttachment bone_attachment;
			serializer.read(bone_attachment.bone_index);
			serializer.read(bone_attachment.entity);
			serializer.read(bone_attachment.parent_entity);
			serializer.read(bone_attachment.relative_transform);
			m_bone_attachments.insert(bone_attachment.entity, bone_attachment);
			m_universe.onComponentCreated(bone_attachment.entity, BONE_ATTACHMENT_TYPE, this);
		}
	}


	void deserializeParticleEmitters(InputBlob& serializer)
	{
		const int count = serializer.read<int>();
		m_particle_emitters.reserve(count);
		for (int i = 0; i < count; ++i) {
			ParticleEmitter* emitter = LUMIX_NEW(m_allocator, ParticleEmitter)(INVALID_ENTITY, m_allocator);
			emitter->deserialize(serializer, m_engine.getResourceManager());
			if(emitter->m_entity.isValid()) {
				m_particle_emitters.insert((EntityRef)emitter->m_entity, emitter);
				m_universe.onComponentCreated((EntityRef)emitter->m_entity, PARTICLE_EMITTER_TYPE, this);
			}
			else {
				LUMIX_DELETE(m_allocator, emitter);
			}
		}
	}


	void serializeParticleEmitters(OutputBlob& serializer)
	{
		serializer.write(m_particle_emitters.size());
		for (auto* emitter : m_particle_emitters)
		{
			emitter->serialize(serializer);
		}
	}


	void serialize(OutputBlob& serializer) override
	{
		serializeCameras(serializer);
		serializeModelInstances(serializer);
		serializeLights(serializer);
		serializeTerrains(serializer);
		serializeParticleEmitters(serializer);
		serializeBoneAttachments(serializer);
		serializeEnvironmentProbes(serializer);
		serializeDecals(serializer);
		serializeTextMeshes(serializer);
	}


	void deserializeCameras(InputBlob& serializer)
	{
		i32 size;
		serializer.read(size);
		m_cameras.rehash(size);
		for (int i = 0; i < size; ++i)
		{
			Camera camera;
			serializer.read(camera.entity);
			serializer.read(camera.far);
			serializer.read(camera.fov);
			serializer.read(camera.is_ortho);
			serializer.read(camera.ortho_size);
			serializer.read(camera.near);

			m_cameras.insert(camera.entity, camera);
			m_universe.onComponentCreated(camera.entity, CAMERA_TYPE, this);
		}
	}

	void deserializeModelInstances(InputBlob& serializer)
	{
        i32 size = 0;
		serializer.read(size);
		m_model_instances.reserve(size);
		for (int i = 0; i < size; ++i)
		{
			auto& r = m_model_instances.emplace();
			serializer.read(r.entity);
			serializer.read(r.flags);
			r.flags.base &= ModelInstance::PERSISTENT_FLAGS;
			ASSERT(r.entity.index == i || !r.entity.isValid());
			r.model = nullptr;
			r.pose = nullptr;
			r.meshes = nullptr;
			r.mesh_count = 0;

			if(r.entity.isValid())
			{
				const EntityRef e = (EntityRef)r.entity;

				u32 path;
				serializer.read(path);

				if (path != 0)
				{
					auto* model = m_engine.getResourceManager().load<Model>(Path(path));
					setModel(e, model);
				}

				int material_count;
				serializer.read(material_count);
				if (material_count > 0)
				{
					allocateCustomMeshes(r, material_count);
					for (int j = 0; j < material_count; ++j)
					{
						char path[MAX_PATH_LENGTH];
						serializer.readString(path, lengthOf(path));
						setModelInstanceMaterial(e, j, Path(path));
					}
				}

				m_universe.onComponentCreated(e, MODEL_INSTANCE_TYPE, this);
			}
		}
	}

	void deserializeLights(InputBlob& serializer)
	{
		i32 size = 0;
		serializer.read(size);
		m_point_lights.resize(size);
		for (int i = 0; i < size; ++i)
		{
			PointLight& light = m_point_lights[i];
			serializer.read(light);
			m_point_lights_map.insert(light.m_entity, i);

			m_universe.onComponentCreated(light.m_entity, POINT_LIGHT_TYPE, this);
		}

		serializer.read(size);
		for (int i = 0; i < size; ++i)
		{
			GlobalLight light;
			serializer.read(light);
			m_global_lights.insert(light.m_entity, light);
			m_universe.onComponentCreated(light.m_entity, GLOBAL_LIGHT_TYPE, this);
		}
		serializer.read(m_active_global_light_entity);
	}

	void deserializeTerrains(InputBlob& serializer)
	{
		i32 size = 0;
		serializer.read(size);
		for (int i = 0; i < size; ++i)
		{
			auto* terrain = LUMIX_NEW(m_allocator, Terrain)(m_renderer, INVALID_ENTITY, *this, m_allocator);
			terrain->deserialize(serializer, m_universe, *this);
			m_terrains.insert(terrain->getEntity(), terrain);
		}
	}


	void deserialize(InputBlob& serializer) override
	{
		deserializeCameras(serializer);
		deserializeModelInstances(serializer);
		deserializeLights(serializer);
		deserializeTerrains(serializer);
		deserializeParticleEmitters(serializer);
		deserializeBoneAttachments(serializer);
		deserializeEnvironmentProbes(serializer);
		deserializeDecals(serializer);
		deserializeTextMeshes(serializer);
	}


	void destroyBoneAttachment(EntityRef entity)
	{
		const BoneAttachment& bone_attachment = m_bone_attachments[entity];
		const EntityPtr parent_entity = bone_attachment.parent_entity;
		if (parent_entity.isValid() && parent_entity.index < m_model_instances.size())
		{
			ModelInstance& mi = m_model_instances[bone_attachment.parent_entity.index];
			mi.flags.unset(ModelInstance::IS_BONE_ATTACHMENT_PARENT);
		}
		m_bone_attachments.erase(entity);
		m_universe.onComponentDestroyed(entity, BONE_ATTACHMENT_TYPE, this);
	}


	void destroyEnvironmentProbe(EntityRef entity)
	{
		auto& probe = m_environment_probes[entity];
		if (probe.texture) probe.texture->getResourceManager().unload(*probe.texture);
		if (probe.irradiance) probe.irradiance->getResourceManager().unload(*probe.irradiance);
		if (probe.radiance) probe.radiance->getResourceManager().unload(*probe.radiance);
		m_environment_probes.erase(entity);
		m_universe.onComponentDestroyed(entity, ENVIRONMENT_PROBE_TYPE, this);
	}


	void destroyModelInstance(EntityRef entity)
	{
		setModel(entity, nullptr);
		auto& model_instance = m_model_instances[entity.index];
		LUMIX_DELETE(m_allocator, model_instance.pose);
		model_instance.pose = nullptr;
		model_instance.entity = INVALID_ENTITY;
		m_universe.onComponentDestroyed(entity, MODEL_INSTANCE_TYPE, this);
	}


	void destroyGlobalLight(EntityRef entity)
	{
		m_universe.onComponentDestroyed(entity, GLOBAL_LIGHT_TYPE, this);

		if ((EntityPtr)entity == m_active_global_light_entity)
		{
			m_active_global_light_entity = INVALID_ENTITY;
		}
		m_global_lights.erase(entity);
	}


	void destroyDecal(EntityRef entity)
	{
		m_decals.erase(entity);
		m_universe.onComponentDestroyed(entity, DECAL_TYPE, this);
	}


	void destroyPointLight(EntityRef entity)
	{
		int index = m_point_lights_map[entity];
		m_point_lights.eraseFast(index);
		m_point_lights_map.erase(entity);
		if (index < m_point_lights.size())
		{
			m_point_lights_map[{m_point_lights[index].m_entity.index}] = index;
		}
		m_universe.onComponentDestroyed(entity, POINT_LIGHT_TYPE, this);
	}


	void destroyTextMesh(EntityRef entity)
	{
		TextMesh* text = m_text_meshes[entity];
		LUMIX_DELETE(m_allocator, text);
		m_text_meshes.erase(entity);
		m_universe.onComponentDestroyed(entity, TEXT_MESH_TYPE, this);
	}


	void destroyCamera(EntityRef entity)
	{
		m_cameras.erase(entity);
		m_universe.onComponentDestroyed(entity, CAMERA_TYPE, this);
		if (m_active_camera == entity) m_active_camera = INVALID_ENTITY;
	}


	void destroyTerrain(EntityRef entity)
	{
		LUMIX_DELETE(m_allocator, m_terrains[entity]);
		m_terrains.erase(entity);
		m_universe.onComponentDestroyed(entity, TERRAIN_TYPE, this);
	}


	void destroyParticleEmitter(EntityRef entity)
	{
		auto* emitter = m_particle_emitters[entity];
		m_universe.onComponentDestroyed((EntityRef)emitter->m_entity, PARTICLE_EMITTER_TYPE, this);
		m_particle_emitters.erase((EntityRef)emitter->m_entity);
		LUMIX_DELETE(m_allocator, emitter);
	}


	void createTextMesh(EntityRef entity)
	{
		TextMesh* text = LUMIX_NEW(m_allocator, TextMesh)(m_allocator);
		m_text_meshes.insert(entity, text);
		m_universe.onComponentCreated(entity, TEXT_MESH_TYPE, this);
	}


	void createCamera(EntityRef entity)
	{
		Camera camera;
		camera.is_ortho = false;
		camera.ortho_size = 10;
		camera.entity = entity;
		camera.fov = Math::degreesToRadians(60);
		camera.screen_width = 800;
		camera.screen_height = 600;
		camera.aspect = 800.0f / 600.0f;
		camera.near = 0.1f;
		camera.far = 10000.0f;
		m_cameras.insert(entity, camera);
		m_universe.onComponentCreated(entity, CAMERA_TYPE, this);

		if (!m_active_camera.isValid()) m_active_camera = entity;
	}


	void createTerrain(EntityRef entity)
	{
		Terrain* terrain = LUMIX_NEW(m_allocator, Terrain)(m_renderer, entity, *this, m_allocator);
		m_terrains.insert(entity, terrain);
		m_universe.onComponentCreated(entity, TERRAIN_TYPE, this);
	}


	void createParticleEmitter(EntityRef entity)
	{
		m_particle_emitters.insert(entity, LUMIX_NEW(m_allocator, ParticleEmitter)(entity, m_allocator));
		m_universe.onComponentCreated(entity, PARTICLE_EMITTER_TYPE, this);
	}


	ModelInstance* getModelInstances() override
	{
		return m_model_instances.empty() ? nullptr : &m_model_instances[0];
	}


	ModelInstance* getModelInstance(EntityRef entity) override
	{
		return &m_model_instances[entity.index];
	}


	Vec3 getPoseBonePosition(EntityRef model_instance, int bone_index)
	{
		Pose* pose = m_model_instances[model_instance.index].pose;
		return pose->positions[bone_index];
	}


	void onEntityDestroyed(EntityRef entity)
	{
		for (auto& i : m_bone_attachments)
		{
			if (i.parent_entity == entity)
			{
				i.parent_entity = INVALID_ENTITY;
				break;
			}
		}
	}


	void onEntityMoved(EntityRef entity)
	{
		int index = entity.index;

		if (index < m_model_instances.size() && m_model_instances[index].entity.isValid() &&
			m_model_instances[index].model && m_model_instances[index].model->isReady())
		{
			ModelInstance& r = m_model_instances[index];
			if (r.model && r.model->isReady())
			{
				float radius = m_universe.getScale(entity) * r.model->getBoundingRadius();
				DVec3 position = m_universe.getPosition(entity);
				m_culling_system->setRadius(entity, radius);
				m_culling_system->setPosition(entity, position);
			}
		}

		int decal_idx = m_decals.find(entity);
		if (decal_idx >= 0)
		{
			updateDecalInfo(m_decals.at(decal_idx));
		}

		bool was_updating = m_is_updating_attachments;
		m_is_updating_attachments = true;
		for (auto& attachment : m_bone_attachments)
		{
			if (attachment.parent_entity == entity)
			{
				updateBoneAttachment(attachment);
			}
		}
		m_is_updating_attachments = was_updating;

		if (m_is_updating_attachments || m_is_game_running) return;
		for (auto& attachment : m_bone_attachments)
		{
			if (attachment.entity == entity)
			{
				updateRelativeMatrix(attachment);
				break;
			}
		}

	}

	Engine& getEngine() const override { return m_engine; }


	Vec2 getTerrainResolution(EntityRef entity) override
	{
		auto* terrain = m_terrains[entity];
		return Vec2((float)terrain->getWidth(), (float)terrain->getHeight());
	}


	EntityPtr getFirstTerrain() override
	{
		if (m_terrains.empty()) return INVALID_ENTITY;
		auto iter = m_terrains.begin();
		return iter.value()->getEntity();
	}


	EntityPtr getNextTerrain(EntityRef entity) override
	{
		auto iter = m_terrains.find(entity);
		++iter;
		if (!iter.isValid()) return INVALID_ENTITY;
		return iter.value()->getEntity();
	}


	Vec3 getTerrainNormalAt(EntityRef entity, float x, float z) override
	{
		return m_terrains[entity]->getNormal(x, z);
	}


	float getTerrainHeightAt(EntityRef entity, float x, float z) override
	{
		return m_terrains[entity]->getHeight(x, z);
	}


	AABB getTerrainAABB(EntityRef entity) override
	{
		return m_terrains[entity]->getAABB();
	}


	Vec2 getTerrainSize(EntityRef entity) override
	{
		return m_terrains[entity]->getSize();
	}


	void setTerrainMaterialPath(EntityRef entity, const Path& path) override
	{
		if (path.isValid())
		{
			Material* material = m_engine.getResourceManager().load<Material>(path);
			m_terrains[entity]->setMaterial(material);
		}
		else
		{
			m_terrains[entity]->setMaterial(nullptr);
		}
	}


	Material* getTerrainMaterial(EntityRef entity) override { return m_terrains[entity]->getMaterial(); }


	void setDecalScale(EntityRef entity, const Vec3& value) override
	{
		Decal& decal = m_decals[entity];
		decal.scale = value;
		updateDecalInfo(decal);
	}


	Vec3 getDecalScale(EntityRef entity) override
	{
		return m_decals[entity].scale;
	}


	void getDecals(const Frustum& frustum, Array<DecalInfo>& decals) override
	{
		/*decals.reserve(m_decals.size());
		for (const Decal& decal : m_decals)
		{
			if (!decal.material || !decal.material->isReady()) continue;
			if (frustum.isSphereInside(decal.position, decal.radius)) decals.push(decal);
		}*/
		// TODO
		ASSERT(false);
	}


	void setDecalMaterialPath(EntityRef entity, const Path& path) override
	{
		Decal& decal = m_decals[entity];
		if (decal.material) {
			decal.material->getResourceManager().unload(*decal.material);
		}

		if (path.isValid()) {
			decal.material = m_engine.getResourceManager().load<Material>(path);
		}
		else {
			decal.material = nullptr;
		}
	}


	Path getDecalMaterialPath(EntityRef entity) override
	{
		Decal& decal = m_decals[entity];
		return decal.material ? decal.material->getPath() : Path("");
	}


	Path getTerrainMaterialPath(EntityRef entity) override
	{
		Terrain* terrain = m_terrains[entity];
		if (terrain->getMaterial())
		{
			return terrain->getMaterial()->getPath();
		}
		else
		{
			return Path("");
		}
	}


	void setTerrainXZScale(EntityRef entity, float scale) override
	{
		m_terrains[entity]->setXZScale(scale);
	}

	float getTerrainXZScale(EntityRef entity) override { return m_terrains[entity]->getXZScale(); }


	void setTerrainYScale(EntityRef entity, float scale) override
	{
		m_terrains[entity]->setYScale(scale);
	}

	float getTerrainYScale(EntityRef entity) override { return m_terrains[entity]->getYScale(); }


	Pose* lockPose(EntityRef entity) override { return m_model_instances[entity.index].pose; }
	void unlockPose(EntityRef entity, bool changed) override
	{
		if (!changed) return;
		if (entity.index < m_model_instances.size()
			&& (m_model_instances[entity.index].flags.isSet(ModelInstance::IS_BONE_ATTACHMENT_PARENT)) == 0)
		{
			return;
		}

		EntityRef parent = entity;
		for (BoneAttachment& ba : m_bone_attachments)
		{
			if (ba.parent_entity != parent) continue;
			m_is_updating_attachments = true;
			updateBoneAttachment(ba);
			m_is_updating_attachments = false;
		}
	}


	Model* getModelInstanceModel(EntityRef entity) override { return m_model_instances[entity.index].model; }


	static u64 getLayerMask(ModelInstance& model_instance)
	{
		Model* model = model_instance.model;
		if (!model->isReady()) return 1;
		u64 layer_mask = 0;
		for(int i = 0; i < model->getMeshCount(); ++i)
		{ 
			layer_mask |= model->getMesh(i).material->getRenderLayerMask();
		}
		return layer_mask;
	}


	bool isModelInstanceEnabled(EntityRef entity) override
	{
		ModelInstance& model_instance = m_model_instances[entity.index];
		return model_instance.flags.isSet(ModelInstance::ENABLED);
	}


	void enableModelInstance(EntityRef entity, bool enable) override
	{
		ModelInstance& model_instance = m_model_instances[entity.index];
		model_instance.flags.set(ModelInstance::ENABLED, enable);
		if (enable)
		{
			if (!model_instance.model || !model_instance.model->isReady()) return;

			const DVec3 pos = m_universe.getPosition((EntityRef)model_instance.entity);
			const float radius = model_instance.model->getBoundingRadius();
			u64 layer_mask = getLayerMask(model_instance);
			if (!m_culling_system->isAdded(entity)) m_culling_system->add(entity, pos, radius, layer_mask);
		}
		else
		{
			m_culling_system->remove(entity);
		}
	}


	Path getModelInstancePath(EntityRef entity) override
	{
		return m_model_instances[entity.index].model ? m_model_instances[entity.index].model->getPath() : Path("");
	}


	int getModelInstanceMaterialsCount(EntityRef entity) override
	{
		return m_model_instances[entity.index].model ? m_model_instances[entity.index].mesh_count : 0;
	}


	void setModelInstancePath(EntityRef entity, const Path& path) override
	{
		ModelInstance& r = m_model_instances[entity.index];

		if (path.isValid()) {
			Model* model = m_engine.getResourceManager().load<Model>(path);
			setModel(entity, model);
		}
		else {
			setModel(entity, nullptr);
		}
	}


	void forceGrassUpdate(EntityRef entity) override { m_terrains[entity]->forceGrassUpdate(); }


	void getTerrainInfos(const Frustum& frustum, const DVec3& lod_ref_point, Array<TerrainInfo>& infos) override
	{
		PROFILE_FUNCTION();
		infos.reserve(m_terrains.size());
		for (auto* terrain : m_terrains)
		{
			terrain->getInfos(infos, frustum, lod_ref_point);
		}
	}


	void getGrassInfos(const Frustum& frustum, EntityRef camera, Array<GrassInfo>& infos) override
	{
		PROFILE_FUNCTION();

		if (!m_is_grass_enabled) return;

		for (auto* terrain : m_terrains)
		{
			terrain->getGrassInfos(frustum, infos, camera);
		}
	}


	static int LUA_castCameraRay(lua_State* L)
	{
		auto* scene = LuaWrapper::checkArg<RenderSceneImpl*>(L, 1);
		EntityRef camera_entity = LuaWrapper::checkArg<EntityRef>(L, 2);
		float x, y;
		if (lua_gettop(L) > 3) {
			x = LuaWrapper::checkArg<float>(L, 3);
			y = LuaWrapper::checkArg<float>(L, 4);
		}
		else {
			x = scene->getCameraScreenWidth(camera_entity) * 0.5f;
			y = scene->getCameraScreenHeight(camera_entity) * 0.5f;
		}

		DVec3 origin;
		Vec3 dir;
		scene->getRay(camera_entity, {x, y}, origin, dir);

		RayCastModelHit hit = scene->castRay(origin, dir, INVALID_ENTITY);
		LuaWrapper::push(L, hit.is_hit);
		LuaWrapper::push(L, hit.is_hit ? hit.origin + hit.dir * hit.t : DVec3(0));

		return 2;
	}


	static float LUA_getTerrainHeightAt(RenderSceneImpl* render_scene, EntityRef entity, int x, int z)
	{
		return render_scene->m_terrains[entity]->getHeight(x, z);
	}


	void setTerrainHeightAt(EntityRef entity, int x, int z, float height)
	{
		m_terrains[entity]->setHeight(x, z, height);
	}

	static u32 LUA_getTexturePixel(Texture* texture, int x, int y)
	{
		if (!texture) return 0;
		if (!texture->isReady()) return 0;
		if (texture->data.empty()) return 0;
		if (texture->bytes_per_pixel != 4) return 0;
		
		x = Math::clamp(x, 0, texture->width - 1);
		y = Math::clamp(y, 0, texture->height - 1);

		return ((u32*)&texture->data[0])[x + y * texture->width];
	}


	static Pipeline* LUA_createPipeline(Engine* engine, const char* path)
	{
		Renderer& renderer = *static_cast<Renderer*>(engine->getPluginManager().getPlugin("renderer"));
		PipelineResource* pres = engine->getResourceManager().load<PipelineResource>(Path(path));
		return Pipeline::create(renderer, pres, "", renderer.getEngine().getAllocator());
	}


	static void LUA_destroyPipeline(Pipeline* pipeline)
	{
		Pipeline::destroy(pipeline);
	}


	static void LUA_setPipelineScene(Pipeline* pipeline, RenderScene* scene)
	{
		pipeline->setScene(scene);
	}


	static RenderScene* LUA_getPipelineScene(Pipeline* pipeline)
	{
		return pipeline->getScene();
	}


	// TODO
	/*
	static bgfx::TextureHandle* LUA_getRenderBuffer(Pipeline* pipeline,
		const char* framebuffer_name,
		int renderbuffer_idx)
	{
bgfx::TextureHandle& handle = pipeline->getRenderbuffer(framebuffer_name, renderbuffer_idx);
		return &handle;
	}*/


	static void LUA_setModelInstancePath(IScene* scene, int component, const char* path)
	{
		RenderScene* render_scene = (RenderScene*)scene;
		render_scene->setModelInstancePath({component}, Path(path));
	}


	static int LUA_getModelBoneIndex(Model* model, const char* bone)
	{
		if (!model) return 0;
		return model->getBoneIndex(crc32(bone)).value();
	}


	static unsigned int LUA_compareTGA(RenderSceneImpl* scene, const char* path, const char* path_preimage, int min_diff)
	{
		auto& fs = scene->m_engine.getFileSystem();
		auto file1 = fs.open(fs.getDefaultDevice(), Path(path), FS::Mode::OPEN_AND_READ);
		auto file2 = fs.open(fs.getDefaultDevice(), Path(path_preimage), FS::Mode::OPEN_AND_READ);
		if (!file1)
		{
			if (file2) fs.close(*file2);
			g_log_error.log("render_test") << "Failed to open " << path;
			return 0xffffFFFF;
		}
		else if (!file2)
		{
			fs.close(*file1);
			g_log_error.log("render_test") << "Failed to open " << path_preimage;
			return 0xffffFFFF;
		}
		unsigned int result = Texture::compareTGA(file1, file2, min_diff, scene->m_allocator);
		fs.close(*file1);
		fs.close(*file2);
		return result;
	}


	static void LUA_makeScreenshot(RenderSceneImpl* scene, const char* path)
	{
		scene->m_renderer.makeScreenshot(Path(path));
	}


	static void LUA_setModelInstanceMaterial(RenderScene* scene,
		EntityRef entity,
		int index,
		const char* path)
	{
		scene->setModelInstanceMaterial(entity, index, Path(path));
	}


	bool isGrassEnabled() const override
	{
		return m_is_grass_enabled;
	}


	int getGrassRotationMode(EntityRef entity, int index) override
	{
		return (int)m_terrains[entity]->getGrassTypeRotationMode(index);
	}


	void setGrassRotationMode(EntityRef entity, int index, int value) override
	{
		m_terrains[entity]->setGrassTypeRotationMode(index, (Terrain::GrassType::RotationMode)value);
	}


	float getGrassDistance(EntityRef entity, int index) override
	{
		return m_terrains[entity]->getGrassTypeDistance(index);
	}


	void setGrassDistance(EntityRef entity, int index, float value) override
	{
		m_terrains[entity]->setGrassTypeDistance(index, value);
	}


	void enableGrass(bool enabled) override { m_is_grass_enabled = enabled; }


	void setGrassDensity(EntityRef entity, int index, int density) override
	{
		m_terrains[entity]->setGrassTypeDensity(index, density);
	}


	int getGrassDensity(EntityRef entity, int index) override
	{
		return m_terrains[entity]->getGrassTypeDensity(index);
	}


	void setGrassPath(EntityRef entity, int index, const Path& path) override
	{
		m_terrains[entity]->setGrassTypePath(index, path);
	}


	Path getGrassPath(EntityRef entity, int index) override
	{
		return m_terrains[entity]->getGrassTypePath(index);
	}


	int getGrassCount(EntityRef entity) override
	{
		return m_terrains[entity]->getGrassTypeCount();
	}


	void addGrass(EntityRef entity, int index) override
	{
		m_terrains[entity]->addGrassType(index);
	}


	void removeGrass(EntityRef entity, int index) override
	{
		m_terrains[entity]->removeGrassType(index);
	}


	EntityPtr getFirstModelInstance() override
	{
		return getNextModelInstance(INVALID_ENTITY);
	}


	EntityPtr getNextModelInstance(EntityPtr entity) override
	{
		for(int i = entity.index + 1; i < m_model_instances.size(); ++i)
		{
			if (m_model_instances[i].entity != INVALID_ENTITY) return {i};
		}
		return INVALID_ENTITY;
	}


	void getPointLights(const Frustum& frustum, Array<EntityRef>& lights) override
	{
		/*for (int i = 0, ci = m_point_lights.size(); i < ci; ++i)
		{
			PointLight& light = m_point_lights[i];

			if (frustum.isSphereInside(m_universe.getPosition(light.m_entity), light.m_range))
			{
				lights.push(light.m_entity);
			}
		}*/
		// TODO
		ASSERT(false);
	}


	void setLightCastShadows(EntityRef entity, bool cast_shadows) override
	{
		m_point_lights[m_point_lights_map[entity]].m_cast_shadows = cast_shadows;
	}


	bool getLightCastShadows(EntityRef entity) override
	{
		return m_point_lights[m_point_lights_map[entity]].m_cast_shadows;
	}


	void getModelInstanceEntities(const ShiftedFrustum& frustum, Array<EntityRef>& entities) override
	{
		PROFILE_FUNCTION();

		CullingSystem::Results results(m_allocator);
		m_culling_system->cull(frustum, ~0ULL, results);

		for (auto& subresults : results)
		{
			for (EntityRef model_instance : subresults)
			{
				entities.push(model_instance);
			}
		}
	}


	float getCameraLODMultiplier(EntityRef camera)
	{
		float lod_multiplier;
		if (isCameraOrtho(camera))
		{
			lod_multiplier = 1;
		}
		else
		{
			lod_multiplier = getCameraFOV(camera) / Math::degreesToRadians(60);
			lod_multiplier *= lod_multiplier;
		}
		return lod_multiplier;
	}


	void getModelInstanceInfos(const ShiftedFrustum& frustum,
		const DVec3& lod_ref_point,
		float lod_multiplier,
		u64 layer_mask,
		Array<MeshInstance>& result) const override
	{
		CullingSystem::Results cull_results(m_allocator);
		m_culling_system->cull(frustum, layer_mask, cull_results);
		if (cull_results.empty()) return;
		
		Array<Array<MeshInstance>> tmp(m_allocator);
		tmp.reserve(cull_results.size());
		for (auto& r : cull_results) {
			tmp.emplace(m_allocator);
		}

		JobSystem::JobDecl jobs[64];
		JobSystem::LambdaJob job_storage[64];
		ASSERT(cull_results.size() <= lengthOf(jobs));

		volatile int counter = 0;
		for (int subresult_index = 0; subresult_index < cull_results.size(); ++subresult_index) {
			Array<MeshInstance>& subinfos = tmp[subresult_index];
			subinfos.reserve(cull_results[subresult_index].size());

			JobSystem::fromLambda([&layer_mask, &subinfos, this, &cull_results, subresult_index, lod_ref_point, lod_multiplier]() {
				PROFILE_BLOCK("Temporary Info Job");
				PROFILE_INT("ModelInstance count", cull_results[subresult_index].size());
				if (cull_results[subresult_index].empty()) return;

				const Universe& universe = m_universe;
				const DVec3 ref_point = lod_ref_point;
				const float final_lod_multiplier = m_lod_multiplier * lod_multiplier;
				const EntityRef* LUMIX_RESTRICT raw_subresults = &cull_results[subresult_index][0];
				const ModelInstance* LUMIX_RESTRICT model_instances = &m_model_instances[0];
				for (int i = 0, c = cull_results[subresult_index].size(); i < c; ++i) {
					const EntityRef e = raw_subresults[i];
					const ModelInstance* LUMIX_RESTRICT model_instance = &model_instances[e.index];
					
					float squared_distance = float((universe.getPosition(e) - ref_point).squaredLength());
					squared_distance *= final_lod_multiplier;

					const Model* LUMIX_RESTRICT model = model_instance->model;
					const LODMeshIndices lod = model->getLODMeshIndices(squared_distance);
					for (int j = lod.from, c = lod.to; j <= c; ++j) {
						const Mesh& mesh = model_instance->meshes[j];
						if ((mesh.layer_mask & layer_mask) == 0) continue;
						
						if (mesh.material->isReady()) {
							MeshInstance& info = subinfos.emplace();
							info.owner = raw_subresults[i];
							info.mesh = &mesh;
							info.depth = squared_distance;
						}
					}
				}
			}, &job_storage[subresult_index], &jobs[subresult_index], &m_allocator);
		}
		JobSystem::runJobs(jobs, cull_results.size(), &counter);
		JobSystem::wait(&counter);

		uint count = 0;
		for (auto& i : tmp) {
			count += i.size();
		}

		result.resize(count);
		uint offset = 0;
		PROFILE_BLOCK("merge temporary infos");
		PROFILE_INT("count", count);
		for (auto& i : tmp) {
			memcpy(result.begin() + offset, i.begin(), i.byte_size());
			offset += i.size();
		}
	}


	float getCameraFOV(EntityRef camera) override { return m_cameras[camera].fov; }
	void setCameraFOV(EntityRef camera, float fov) override { m_cameras[camera].fov = fov; }
	void setCameraNearPlane(EntityRef camera, float near_plane) override { m_cameras[camera].near = Math::maximum(near_plane, 0.00001f); }
	float getCameraNearPlane(EntityRef camera) override { return m_cameras[camera].near; }
	void setCameraFarPlane(EntityRef camera, float far_plane) override { m_cameras[camera].far = far_plane; }
	float getCameraFarPlane(EntityRef camera) override { return m_cameras[camera].far; }
	float getCameraScreenWidth(EntityRef camera) override { return m_cameras[camera].screen_width; }
	float getCameraScreenHeight(EntityRef camera) override { return m_cameras[camera].screen_height; }


	void setGlobalLODMultiplier(float multiplier) { m_lod_multiplier = multiplier; }
	float getGlobalLODMultiplier() const { return m_lod_multiplier; }


	Matrix getCameraViewProjection(EntityRef entity) override
	{
		/*Matrix view = m_universe.getMatrix(entity);
		view.fastInverse();
		return getCameraProjection(entity) * view;*/
		// TODO
		ASSERT(false);
		return {};
	}


	Matrix getCameraProjection(EntityRef entity) override
	{
		const Camera& camera = m_cameras[entity];
		Matrix mtx;
		const float ratio = camera.screen_height > 0 ? camera.screen_width / camera.screen_height : 1;
		const bool is_homogenous_depth = ffr::isHomogenousDepth();
		if (camera.is_ortho) {
			mtx.setOrtho(-camera.ortho_size * ratio,
				camera.ortho_size * ratio,
				-camera.ortho_size,
				camera.ortho_size,
				camera.near,
				camera.far,
				is_homogenous_depth,
				true);
		}
		else {
			mtx.setPerspective(camera.fov, ratio, camera.near, camera.far, is_homogenous_depth, true);
		}
		return mtx;
	}


	void setCameraScreenSize(EntityRef camera, int w, int h) override
	{
		auto& cam = m_cameras[{camera.index}];
		cam.screen_width = (float)w;
		cam.screen_height = (float)h;
		cam.aspect = w / (float)h;
	}


	Vec2 getCameraScreenSize(EntityRef camera) override
	{
		auto& cam = m_cameras[{camera.index}];
		return Vec2(cam.screen_width, cam.screen_height);
	}


	float getCameraOrthoSize(EntityRef camera) override { return m_cameras[{camera.index}].ortho_size; }
	void setCameraOrthoSize(EntityRef camera, float value) override { m_cameras[{camera.index}].ortho_size = value; }
	bool isCameraOrtho(EntityRef camera) override { return m_cameras[{camera.index}].is_ortho; }
	void setCameraOrtho(EntityRef camera, bool is_ortho) override { m_cameras[{camera.index}].is_ortho = is_ortho; }


	const Array<DebugTriangle>& getDebugTriangles() const override { return m_debug_triangles; }
	const Array<DebugLine>& getDebugLines() const override { return m_debug_lines; }
	const Array<DebugPoint>& getDebugPoints() const override { return m_debug_points; }


	void addDebugSphere(const DVec3& center,
		float radius,
		u32 color,
		float life) override
	{
		static const int COLS = 36;
		static const int ROWS = COLS >> 1;
		static const float STEP = (Math::PI / 180.0f) * 360.0f / COLS;
		int p2 = COLS >> 1;
		int r2 = ROWS >> 1;
		float prev_ci = 1;
		float prev_si = 0;
		for (int y = -r2; y < r2; ++y)
		{
			float cy = cos(y * STEP);
			float cy1 = cos((y + 1) * STEP);
			float sy = sin(y * STEP);
			float sy1 = sin((y + 1) * STEP);

			for (int i = -p2; i < p2; ++i)
			{
				float ci = cos(i * STEP);
				float si = sin(i * STEP);
				addDebugLine(DVec3(center.x + radius * ci * cy,
								  center.y + radius * sy,
								  center.z + radius * si * cy),
							 DVec3(center.x + radius * ci * cy1,
								  center.y + radius * sy1,
								  center.z + radius * si * cy1),
							 color,
							 life);
				addDebugLine(DVec3(center.x + radius * ci * cy,
								  center.y + radius * sy,
								  center.z + radius * si * cy),
							 DVec3(center.x + radius * prev_ci * cy,
								  center.y + radius * sy,
								  center.z + radius * prev_si * cy),
							 color,
							 life);
				addDebugLine(DVec3(center.x + radius * prev_ci * cy1,
								  center.y + radius * sy1,
								  center.z + radius * prev_si * cy1),
							 DVec3(center.x + radius * ci * cy1,
								  center.y + radius * sy1,
								  center.z + radius * si * cy1),
							 color,
							 life);
				prev_ci = ci;
				prev_si = si;
			}
		}
	}


	void addDebugHalfSphere(const RigidTransform& transform, float radius, bool top, u32 color, float life)
	{
		const DVec3 center = transform.pos;
		const Vec3 x_vec = transform.rot * Vec3(1, 0, 0);
		const Vec3 y_vec = transform.rot * Vec3(0, top ? 1.f : -1.f, 0);
		const Vec3 z_vec = transform.rot * Vec3(0, 0, 1);
		static const int COLS = 36;
		static const int ROWS = COLS >> 1;
		static const float STEP = Math::degreesToRadians(360.0f) / COLS;
		for (int y = 0; y < ROWS >> 1; ++y)
		{
			float cy = cos(y * STEP);
			float cy1 = cos((y + 1) * STEP);
			float sy = sin(y * STEP);
			float sy1 = sin((y + 1) * STEP);
			float prev_ci = cos(-STEP);
			float prev_si = sin(-STEP);

			Vec3 y_offset = y_vec * sy;
			Vec3 y_offset1 = y_vec * sy1;

			for (int i = 0; i < COLS; ++i)
			{
				float ci = cos(i * STEP);
				float si = sin(i * STEP);

				addDebugLine(
					center + radius * (x_vec * ci * cy + z_vec * si * cy + y_offset),
					center + radius * (x_vec * prev_ci * cy + z_vec * prev_si * cy + y_offset),
					color,
					life);
				addDebugLine(
					center + radius * (x_vec * ci * cy + z_vec * si * cy + y_offset),
					center + radius * (x_vec * ci * cy1 + z_vec * si * cy1 + y_offset1),
					color,
					life);
				prev_ci = ci;
				prev_si = si;
			}
		}
	}


	void addDebugHalfSphere(const DVec3& center, float radius, bool top, u32 color, float life)
	{
		static const int COLS = 36;
		static const int ROWS = COLS >> 1;
		static const float STEP = (Math::PI / 180.0f) * 360.0f / COLS;
		int p2 = COLS >> 1;
		int yfrom = top ? 0 : -(ROWS >> 1);
		int yto = top ? ROWS >> 1 : 0;
		for (int y = yfrom; y < yto; ++y)
		{
			float cy = cos(y * STEP);
			float cy1 = cos((y + 1) * STEP);
			float sy = sin(y * STEP);
			float sy1 = sin((y + 1) * STEP);
			float prev_ci = cos((-p2 - 1) * STEP);
			float prev_si = sin((-p2 - 1) * STEP);

			for (int i = -p2; i < p2; ++i)
			{
				float ci = cos(i * STEP);
				float si = sin(i * STEP);
				addDebugLine(DVec3(center.x + radius * ci * cy,
					center.y + radius * sy,
					center.z + radius * si * cy),
					DVec3(center.x + radius * ci * cy1,
					center.y + radius * sy1,
					center.z + radius * si * cy1),
					color,
					life);
				addDebugLine(DVec3(center.x + radius * ci * cy,
					center.y + radius * sy,
					center.z + radius * si * cy),
					DVec3(center.x + radius * prev_ci * cy,
					center.y + radius * sy,
					center.z + radius * prev_si * cy),
					color,
					life);
				addDebugLine(DVec3(center.x + radius * prev_ci * cy1,
					center.y + radius * sy1,
					center.z + radius * prev_si * cy1),
					DVec3(center.x + radius * ci * cy1,
					center.y + radius * sy1,
					center.z + radius * si * cy1),
					color,
					life);
				prev_ci = ci;
				prev_si = si;
			}
		}
	}


	void addDebugTriangle(const DVec3& p0,
		const DVec3& p1,
		const DVec3& p2,
		u32 color,
		float life) override
	{
		DebugTriangle& tri = m_debug_triangles.emplace();
		tri.p0 = p0;
		tri.p1 = p1;
		tri.p2 = p2;
		tri.color = ARGBToABGR(color);
		tri.life = life;
	}


	void addDebugCapsule(const DVec3& position,
		float height,
		float radius,
		u32 color,
		float life) override
	{
		addDebugHalfSphere(position + Vec3(0, radius, 0), radius, false, color, life);
		addDebugHalfSphere(position + Vec3(0, radius + height, 0), radius, true, color, life);

		Vec3 z_vec(0, 0, 1.0f);
		Vec3 x_vec(1.0f, 0, 0);
		z_vec.normalize();
		x_vec.normalize();
		const DVec3 bottom = position + Vec3(0, radius, 0);
		const DVec3 top = bottom + Vec3(0, height, 0);
		for (int i = 1; i <= 32; ++i)
		{
			const float a = i / 32.0f * 2 * Math::PI;
			const float x = cosf(a) * radius;
			const float z = sinf(a) * radius;
			addDebugLine(bottom + x_vec * x + z_vec * z,
				top + x_vec * x + z_vec * z,
				color,
				life);
		}
	}


	void addDebugCapsule(const Matrix& transform,
		float height,
		float radius,
		u32 color,
		float life) override
	{
		/*Vec3 x_vec = transform.getXVector();
		Vec3 y_vec = transform.getYVector();
		Vec3 z_vec = transform.getZVector();
		Vec3 position = transform.getTranslation();

		Matrix tmp = transform;
		tmp.setTranslation(transform.getTranslation() + y_vec * radius);
		addDebugHalfSphere(tmp, radius, false, color, life);
		tmp.setTranslation(transform.getTranslation() + y_vec * (radius + height));
		addDebugHalfSphere(tmp, radius, true, color, life);

		Vec3 bottom = position + y_vec * radius;
		Vec3 top = bottom + y_vec * height;
		for (int i = 1; i <= 32; ++i)
		{
			float a = i / 32.0f * 2 * Math::PI;
			float x = cosf(a) * radius;
			float z = sinf(a) * radius;
			addDebugLine(bottom + x_vec * x + z_vec * z, top + x_vec * x + z_vec * z, color, life);
		}*/
		// TODO
		ASSERT(false);
	}



	void addDebugCylinder(const DVec3& position,
								  const Vec3& up,
								  float radius,
								  u32 color,
								  float life) override
	{
		Vec3 z_vec(-up.y, up.x, 0);
		Vec3 x_vec = crossProduct(up, z_vec);
		float prevx = radius;
		float prevz = 0;
		z_vec.normalize();
		x_vec.normalize();
		const DVec3 top = position + up;
		for (int i = 1; i <= 32; ++i)
		{
			float a = i / 32.0f * 2 * Math::PI;
			float x = cosf(a) * radius;
			float z = sinf(a) * radius;
			addDebugLine(position + x_vec * x + z_vec * z,
						 position + x_vec * prevx + z_vec * prevz,
						 color,
						 life);
			addDebugLine(top + x_vec * x + z_vec * z,
						 top + x_vec * prevx + z_vec * prevz,
						 color,
						 life);
			addDebugLine(position + x_vec * x + z_vec * z,
						 top + x_vec * x + z_vec * z,
						 color,
						 life);
			prevx = x;
			prevz = z;
		}
	}


	void addDebugCube(const DVec3& pos,
		const Vec3& dir,
		const Vec3& up,
		const Vec3& right,
		u32 color,
		float life) override
	{
		addDebugLine(pos + dir + up + right, pos + dir + up - right, color, life);
		addDebugLine(pos - dir + up + right, pos - dir + up - right, color, life);
		addDebugLine(pos + dir + up + right, pos - dir + up + right, color, life);
		addDebugLine(pos + dir + up - right, pos - dir + up - right, color, life);

		addDebugLine(pos + dir - up + right, pos + dir - up - right, color, life);
		addDebugLine(pos - dir - up + right, pos - dir - up - right, color, life);
		addDebugLine(pos + dir - up + right, pos - dir - up + right, color, life);
		addDebugLine(pos + dir - up - right, pos - dir - up - right, color, life);

		addDebugLine(pos + dir + up + right, pos + dir - up + right, color, life);
		addDebugLine(pos + dir + up - right, pos + dir - up - right, color, life);
		addDebugLine(pos - dir + up + right, pos - dir - up + right, color, life);
		addDebugLine(pos - dir + up - right, pos - dir - up - right, color, life);

	}


	void addDebugCubeSolid(const DVec3& min,
		const DVec3& max,
		u32 color,
		float life) override
	{
		DVec3 a = min;
		DVec3 b = min;
		DVec3 c = max;

		b.x = max.x;
		c.z = min.z;
		addDebugTriangle(a, c, b, color, life);
		b.x = min.x;
		b.y = max.y;
		addDebugTriangle(a, b, c, color, life);

		b = max;
		c = max;
		a.z = max.z;
		b.y = min.y;
		addDebugTriangle(a, b, c, color, life);
		b.x = min.x;
		b.y = max.y;
		addDebugTriangle(a, c, b, color, life);

		a = min;
		b = min;
		c = max;

		b.x = max.x;
		c.y = min.y;
		addDebugTriangle(a, b, c, color, life);
		b.x = min.x;
		b.z = max.z;
		addDebugTriangle(a, c, b, color, life);

		b = max;
		c = max;
		a.y = max.y;
		b.z = min.z;
		addDebugTriangle(a, c, b, color, life);
		b.x = min.x;
		b.z = max.z;
		addDebugTriangle(a, b, c, color, life);

		a = min;
		b = min;
		c = max;

		b.y = max.y;
		c.x = min.x;
		addDebugTriangle(a, c, b, color, life);
		b.y = min.y;
		b.z = max.z;
		addDebugTriangle(a, b, c, color, life);

		b = max;
		c = max;
		a.x = max.x;
		b.z = min.z;
		addDebugTriangle(a, b, c, color, life);
		b.y = min.y;
		b.z = max.z;
		addDebugTriangle(a, c, b, color, life);
	}



	void addDebugCube(const DVec3& min,
		const DVec3& max,
		u32 color,
		float life) override
	{
		DVec3 a = min;
		DVec3 b = min;
		b.x = max.x;
		addDebugLine(a, b, color, life);
		a = DVec3(b.x, b.y, max.z);
		addDebugLine(a, b, color, life);
		b = DVec3(min.x, a.y, a.z);
		addDebugLine(a, b, color, life);
		a = DVec3(b.x, b.y, min.z);
		addDebugLine(a, b, color, life);

		a = min;
		a.y = max.y;
		b = a;
		b.x = max.x;
		addDebugLine(a, b, color, life);
		a = DVec3(b.x, b.y, max.z);
		addDebugLine(a, b, color, life);
		b = DVec3(min.x, a.y, a.z);
		addDebugLine(a, b, color, life);
		a = DVec3(b.x, b.y, min.z);
		addDebugLine(a, b, color, life);

		a = min;
		b = a;
		b.y = max.y;
		addDebugLine(a, b, color, life);
		a.x = max.x;
		b.x = max.x;
		addDebugLine(a, b, color, life);
		a.z = max.z;
		b.z = max.z;
		addDebugLine(a, b, color, life);
		a.x = min.x;
		b.x = min.x;
		addDebugLine(a, b, color, life);
	}


	void addDebugFrustum(const ShiftedFrustum& frustum, u32 color, float life) override
	{
		const DVec3 o = frustum.origin;
		addDebugLine(o + frustum.points[0], o + frustum.points[1], color, life);
		addDebugLine(o + frustum.points[1], o + frustum.points[2], color, life);
		addDebugLine(o + frustum.points[2], o + frustum.points[3], color, life);
		addDebugLine(o + frustum.points[3], o + frustum.points[0], color, life);

		addDebugLine(o + frustum.points[4], o + frustum.points[5], color, life);
		addDebugLine(o + frustum.points[5], o + frustum.points[6], color, life);
		addDebugLine(o + frustum.points[6], o + frustum.points[7], color, life);
		addDebugLine(o + frustum.points[7], o + frustum.points[4], color, life);

		addDebugLine(o + frustum.points[0], o + frustum.points[4], color, life);
		addDebugLine(o + frustum.points[1], o + frustum.points[5], color, life);
		addDebugLine(o + frustum.points[2], o + frustum.points[6], color, life);
		addDebugLine(o + frustum.points[3], o + frustum.points[7], color, life);
	}


	void addDebugCircle(const DVec3& center, const Vec3& up, float radius, u32 color, float life) override
	{
		Vec3 z_vec(-up.y, up.x, 0);
		Vec3 x_vec = crossProduct(up, z_vec);
		float prevx = radius;
		float prevz = 0;
		z_vec.normalize();
		x_vec.normalize();
		for (int i = 1; i <= 64; ++i)
		{
			float a = i / 64.0f * 2 * Math::PI;
			float x = cosf(a) * radius;
			float z = sinf(a) * radius;
			addDebugLine(center + x_vec * x + z_vec * z, center + x_vec * prevx + z_vec * prevz, color, life);
			prevx = x;
			prevz = z;
		}
	}


	void addDebugCross(const DVec3& center, float size, u32 color, float life) override
	{
		addDebugLine(center, DVec3(center.x - size, center.y, center.z), color, life);
		addDebugLine(center, DVec3(center.x + size, center.y, center.z), color, life);
		addDebugLine(center, DVec3(center.x, center.y - size, center.z), color, life);
		addDebugLine(center, DVec3(center.x, center.y + size, center.z), color, life);
		addDebugLine(center, DVec3(center.x, center.y, center.z - size), color, life);
		addDebugLine(center, DVec3(center.x, center.y, center.z + size), color, life);
	}


	void addDebugPoint(const DVec3& pos, u32 color, float life) override
	{
		DebugPoint& point = m_debug_points.emplace();
		point.pos = pos;
		point.color = ARGBToABGR(color);
		point.life = life;
	}


	void addDebugCone(const DVec3& vertex,
		const Vec3& dir,
		const Vec3& axis0,
		const Vec3& axis1,
		u32 color,
		float life) override
	{
		const DVec3 base_center = vertex + dir;
		DVec3 prev_p = base_center + axis0;
		for (int i = 1; i <= 32; ++i)
		{
			float angle = i / 32.0f * 2 * Math::PI;
			const Vec3 x = cosf(angle) * axis0;
			const Vec3 z = sinf(angle) * axis1;
			const DVec3 p = base_center + x + z;
			addDebugLine(p, prev_p, color, life);
			addDebugLine(vertex, p, color, life);
			prev_p = p;
		}
	}


	static u32 ARGBToABGR(u32 color)
	{
		return ((color & 0xff) << 16) | (color & 0xff00) | ((color & 0xff0000) >> 16) | (color & 0xff000000);
	}


	void addDebugLine(const DVec3& from, const DVec3& to, u32 color, float life) override
	{
		DebugLine& line = m_debug_lines.emplace();
		line.from = from;
		line.to = to;
		line.color = ARGBToABGR(color);
		line.life = life;
	}


	RayCastModelHit castRayTerrain(EntityRef entity, const DVec3& origin, const Vec3& dir) override
	{
		RayCastModelHit hit;
		hit.is_hit = false;
		auto iter = m_terrains.find(entity);
		if (!iter.isValid()) return hit;

		Terrain* terrain = iter.value();
		hit = terrain->castRay(origin, dir);
		hit.component_type = TERRAIN_TYPE;
		hit.entity = terrain->getEntity();
		return hit;
	}


	RayCastModelHit castRay(const DVec3& origin, const Vec3& dir, EntityPtr ignored_model_instance) override
	{
		PROFILE_FUNCTION();
		RayCastModelHit hit;
		hit.is_hit = false;
		hit.origin = origin;
		hit.dir = dir;
		double cur_dist = DBL_MAX;
		const Universe& universe = getUniverse();
		for (int i = 0; i < m_model_instances.size(); ++i) {
			auto& r = m_model_instances[i];
			if (ignored_model_instance.index == i || !r.model) continue;
			if (!r.flags.isSet(ModelInstance::ENABLED)) continue;

			const EntityRef entity = (EntityRef)r.entity;
			const DVec3& pos = universe.getPosition(entity);
			float scale = universe.getScale(entity);
			float radius = r.model->getBoundingRadius() * scale;
			const double dist = (pos - origin).length();
			if (dist - radius > cur_dist) continue;
			
			Vec3 intersection;
			const Vec3 rel_pos = (origin - pos).toFloat();
			if (Math::getRaySphereIntersection(rel_pos, dir, Vec3::ZERO, radius, intersection)) {
				RayCastModelHit new_hit = r.model->castRay(rel_pos, dir, r.pose);
				if (new_hit.is_hit && (!hit.is_hit || new_hit.t < hit.t)) {
					new_hit.entity = entity;
					new_hit.component_type = MODEL_INSTANCE_TYPE;
					hit = new_hit;
					hit.is_hit = true;
					cur_dist = dir.length() * hit.t;
				}
			}
		}

		for (auto* terrain : m_terrains) {
			RayCastModelHit terrain_hit = terrain->castRay(origin, dir);
			if (terrain_hit.is_hit && (!hit.is_hit || terrain_hit.t < hit.t)) {
				terrain_hit.component_type = TERRAIN_TYPE;
				terrain_hit.entity = terrain->getEntity();
				terrain_hit.mesh = nullptr;
				hit = terrain_hit;
			}
		}

		return hit;
	}

	
	Vec4 getShadowmapCascades(EntityRef entity) override
	{
		return m_global_lights[entity].m_cascades;
	}


	void setShadowmapCascades(EntityRef entity, const Vec4& value) override
	{
		Vec4 valid_value = value;
		valid_value.x = Math::maximum(valid_value.x, 0.02f);
		valid_value.y = Math::maximum(valid_value.x + 0.01f, valid_value.y);
		valid_value.z = Math::maximum(valid_value.y + 0.01f, valid_value.z);
		valid_value.w = Math::maximum(valid_value.z + 0.01f, valid_value.w);

		m_global_lights[entity].m_cascades = valid_value;
	}


	void setFogDensity(EntityRef entity, float density) override
	{
		m_global_lights[entity].m_fog_density = density;
	}


	void setFogColor(EntityRef entity, const Vec3& color) override
	{
		m_global_lights[entity].m_fog_color = color;
	}


	float getFogDensity(EntityRef entity) override
	{
		return m_global_lights[entity].m_fog_density;
	}


	float getFogBottom(EntityRef entity) override
	{
		return m_global_lights[entity].m_fog_bottom;
	}


	void setFogBottom(EntityRef entity, float bottom) override
	{
		m_global_lights[entity].m_fog_bottom = bottom;
	}


	float getFogHeight(EntityRef entity) override
	{
		return m_global_lights[entity].m_fog_height;
	}


	void setFogHeight(EntityRef entity, float height) override
	{
		m_global_lights[entity].m_fog_height = height;
	}


	Vec3 getFogColor(EntityRef entity) override
	{
		return m_global_lights[entity].m_fog_color;
	}


	float getLightAttenuation(EntityRef entity) override
	{
		return m_point_lights[m_point_lights_map[entity]].m_attenuation_param;
	}


	void setLightAttenuation(EntityRef entity, float attenuation) override
	{
		int index = m_point_lights_map[entity];
		m_point_lights[index].m_attenuation_param = attenuation;
	}


	float getLightRange(EntityRef entity) override
	{
		return m_point_lights[m_point_lights_map[entity]].m_range;
	}


	void setLightRange(EntityRef entity, float value) override
	{
		m_point_lights[m_point_lights_map[entity]].m_range = value;
	}


	void setPointLightIntensity(EntityRef entity, float intensity) override
	{
		m_point_lights[m_point_lights_map[entity]].m_diffuse_intensity = intensity;
	}


	void setGlobalLightIntensity(EntityRef entity, float intensity) override
	{
		m_global_lights[entity].m_diffuse_intensity = intensity;
	}


	void setGlobalLightIndirectIntensity(EntityRef entity, float intensity) override
	{
		m_global_lights[entity].m_indirect_intensity = intensity;
	}


	void setPointLightColor(EntityRef entity, const Vec3& color) override
	{
		m_point_lights[m_point_lights_map[entity]].m_diffuse_color = color;
	}


	void setGlobalLightColor(EntityRef entity, const Vec3& color) override
	{
		m_global_lights[entity].m_diffuse_color = color;
	}

	
	float getPointLightIntensity(EntityRef entity) override
	{
		return m_point_lights[m_point_lights_map[entity]].m_diffuse_intensity;
	}


	float getGlobalLightIntensity(EntityRef entity) override
	{
		return m_global_lights[entity].m_diffuse_intensity;
	}


	float getGlobalLightIndirectIntensity(EntityRef entity) override
	{
		return m_global_lights[entity].m_indirect_intensity;
	}


	Vec3 getPointLightColor(EntityRef entity) override
	{
		return m_point_lights[m_point_lights_map[entity]].m_diffuse_color;
	}


	void setPointLightSpecularColor(EntityRef entity, const Vec3& color) override
	{
		m_point_lights[m_point_lights_map[entity]].m_specular_color = color;
	}


	Vec3 getPointLightSpecularColor(EntityRef entity) override
	{
		return m_point_lights[m_point_lights_map[entity]].m_specular_color;
	}


	void setPointLightSpecularIntensity(EntityRef entity, float intensity) override
	{
		m_point_lights[m_point_lights_map[entity]].m_specular_intensity = intensity;
	}


	float getPointLightSpecularIntensity(EntityRef entity) override
	{
		return m_point_lights[m_point_lights_map[entity]].m_specular_intensity;
	}


	Vec3 getGlobalLightColor(EntityRef entity) override
	{
		return m_global_lights[entity].m_diffuse_color;
	}


	void setActiveGlobalLight(EntityRef entity) override
	{
		m_active_global_light_entity = entity;
	}


	EntityPtr getActiveGlobalLight() override
	{
		return m_active_global_light_entity;
	}


	EntityRef getPointLightEntity(EntityRef entity) const override
	{
		return m_point_lights[m_point_lights_map[entity]].m_entity;
	}


	EntityRef getGlobalLightEntity(EntityRef entity) const override
	{
		return m_global_lights[entity].m_entity;
	}


	void reloadEnvironmentProbe(EntityRef entity) override
	{
		auto& probe = m_environment_probes[entity];
		ResourceManagerHub& rm = m_engine.getResourceManager();
		if (probe.texture) probe.texture->getResourceManager().unload(*probe.texture);
		probe.texture = nullptr;
		StaticString<MAX_PATH_LENGTH> path;
		if (probe.flags.isSet(EnvironmentProbe::REFLECTION)) {
			path  << "universes/" << m_universe.getName() << "/probes/" << probe.guid << ".dds";
			probe.texture = rm.load<Texture>(Path(path));
			// TODO
			//probe.texture->setFlag(BGFX_TEXTURE_SRGB, true);
		}
		path = "universes/";
		path << m_universe.getName() << "/probes/" << probe.guid << "_irradiance.dds";
		if(probe.irradiance) probe.irradiance->getResourceManager().unload(*probe.irradiance);
		probe.irradiance = rm.load<Texture>(Path(path));
		//probe.irradiance->setFlag(BGFX_TEXTURE_SRGB, true);
		//probe.irradiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
		//probe.irradiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);
		path = "universes/";
		path << m_universe.getName() << "/probes/" << probe.guid << "_radiance.dds";
		if (probe.radiance) probe.irradiance->getResourceManager().unload(*probe.radiance);
		probe.radiance = rm.load<Texture>(Path(path));
		//probe.radiance->setFlag(BGFX_TEXTURE_SRGB, true);
		//probe.radiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
		//probe.radiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);
	}

	
	void getEnvironmentProbes(Array<EnvProbeInfo>& probes) override
	{
		PROFILE_FUNCTION();
		probes.reserve(m_environment_probes.size());
		for (int i = 0; i < m_environment_probes.size(); ++i) {
			const EnvironmentProbe& probe = m_environment_probes.at(i);
			const EntityRef entity = m_environment_probes.getKey(i);
			if(!probe.flags.isSet(EnvironmentProbe::ENABLED)) continue;
			
			EnvProbeInfo& out = probes.emplace();
			out.radius = probe.radius;
			out.position = m_universe.getPosition(entity);
			out.radiance = probe.radiance && probe.radiance->isReady() ? probe.radiance->handle : ffr::INVALID_TEXTURE;
			out.irradiance = probe.irradiance && probe.irradiance->isReady() ? probe.irradiance->handle : ffr::INVALID_TEXTURE;
			out.reflection = probe.texture && probe.texture->isReady() ? probe.texture->handle : ffr::INVALID_TEXTURE;
		}
	}
		
	
	void enableEnvironmentProbe(EntityRef entity, bool enable) override
	{
		return m_environment_probes[entity].flags.set(EnvironmentProbe::ENABLED, enable);
	}


	bool isEnvironmentProbeEnabled(EntityRef entity) override
	{
		return m_environment_probes[entity].flags.isSet(EnvironmentProbe::ENABLED);
	}


	int getEnvironmentProbeIrradianceSize(EntityRef entity) override
	{
		return m_environment_probes[entity].irradiance_size;
	}


	void setEnvironmentProbeIrradianceSize(EntityRef entity, int size) override
	{
		m_environment_probes[entity].irradiance_size = size;
	}


	float getEnvironmentProbeRadius(EntityRef entity) override
	{
		return m_environment_probes[entity].radius;
	}


	void setEnvironmentProbeRadius(EntityRef entity, float radius) override
	{
		m_environment_probes[entity].radius = radius;
	}


	int getEnvironmentProbeRadianceSize(EntityRef entity) override
	{
		return m_environment_probes[entity].radiance_size;
	}


	void setEnvironmentProbeRadianceSize(EntityRef entity, int size) override
	{
		m_environment_probes[entity].radiance_size = size;
	}


	int getEnvironmentProbeReflectionSize(EntityRef entity) override
	{
		return m_environment_probes[entity].reflection_size;
	}


	void setEnvironmentProbeReflectionSize(EntityRef entity, int size) override
	{
		m_environment_probes[entity].reflection_size = size;
	}


	bool isEnvironmentProbeCustomSize(EntityRef entity) override
	{
		return m_environment_probes[entity].flags.isSet(EnvironmentProbe::OVERRIDE_GLOBAL_SIZE);
	}


	void enableEnvironmentProbeCustomSize(EntityRef entity, bool enable) override
	{
		m_environment_probes[entity].flags.set(EnvironmentProbe::OVERRIDE_GLOBAL_SIZE, enable);
	}


	bool isEnvironmentProbeReflectionEnabled(EntityRef entity) override
	{
		return m_environment_probes[entity].flags.isSet(EnvironmentProbe::REFLECTION);
	}


	void enableEnvironmentProbeReflection(EntityRef entity, bool enable) override
	{
		m_environment_probes[entity].flags.set(EnvironmentProbe::REFLECTION, enable);
	}


	Texture* getEnvironmentProbeTexture(EntityRef entity) const override
	{
		return m_environment_probes[entity].texture;
	}


	Texture* getEnvironmentProbeIrradiance(EntityRef entity) const override
	{
		return m_environment_probes[entity].irradiance;
	}


	Texture* getEnvironmentProbeRadiance(EntityRef entity) const override
	{
		return m_environment_probes[entity].radiance;
	}


	u64 getEnvironmentProbeGUID(EntityRef entity) const override
	{
		return m_environment_probes[entity].guid;
	}


	float getTime() const override { return m_time; }


	void modelUnloaded(Model*, EntityRef entity)
	{
		auto& r = m_model_instances[entity.index];
		if (!hasCustomMeshes(r))
		{
			r.meshes = nullptr;
			r.mesh_count = 0;
		}
		LUMIX_DELETE(m_allocator, r.pose);
		r.pose = nullptr;

		m_culling_system->remove(entity);
	}


	void freeCustomMeshes(ModelInstance& r, MaterialManager* manager)
	{
		if (!hasCustomMeshes(r)) return;
		for (int i = 0; i < r.mesh_count; ++i)
		{
			removeFromCustomMeshCallbacks(r.meshes[i].material, (EntityRef)r.entity);
			manager->unload(*r.meshes[i].material);
			r.meshes[i].~Mesh();
		}
		m_allocator.deallocate(r.meshes);
		r.meshes = nullptr;
		r.flags.unset(ModelInstance::CUSTOM_MESHES);
		r.mesh_count = 0;
	}


	void modelLoaded(Model* model, EntityRef entity)
	{
		auto& rm = m_engine.getResourceManager();
		auto* material_manager = static_cast<MaterialManager*>(rm.get(Material::TYPE));

		auto& r = m_model_instances[entity.index];

		float bounding_radius = r.model->getBoundingRadius();
		float scale = m_universe.getScale(entity);
		const DVec3 pos = m_universe.getPosition(entity);
		const float radius = bounding_radius * scale;
		if(r.flags.isSet(ModelInstance::ENABLED)) m_culling_system->add(entity, pos, radius, getLayerMask(r));
		ASSERT(!r.pose);
		if (model->getBoneCount() > 0)
		{
			r.pose = LUMIX_NEW(m_allocator, Pose)(m_allocator);
			r.pose->resize(model->getBoneCount());
			model->getPose(*r.pose);
			int skinned_define_idx = m_renderer.getShaderDefineIdx("SKINNED");
			for (int i = 0; i < model->getMeshCount(); ++i)
			{
				Mesh& mesh = model->getMesh(i);
				mesh.material->setDefine(skinned_define_idx, !mesh.skin.empty());
			}
		}
		ASSERT(!r.meshes || hasCustomMeshes(r));
		if (r.meshes)
		{
			allocateCustomMeshes(r, model->getMeshCount());
			for (int i = 0; i < r.mesh_count; ++i)
			{
				auto& src = model->getMesh(i);
				if (!r.meshes[i].material)
				{
					material_manager->load(*src.material);
					r.meshes[i].material = src.material;
				}
				r.meshes[i].set(src);
			}
		}
		else
		{
			r.meshes = &r.model->getMesh(0);
			r.mesh_count = r.model->getMeshCount();
		}

		if (r.flags.isSet(ModelInstance::IS_BONE_ATTACHMENT_PARENT))
		{
			updateBoneAttachment(m_bone_attachments[entity]);
		}
	}


	void modelUnloaded(Model* model)
	{
		for (int i = 0, c = m_model_instances.size(); i < c; ++i)
		{
			if (m_model_instances[i].entity != INVALID_ENTITY && m_model_instances[i].model == model)
			{
				modelUnloaded(model, {i});
			}
		}
	}


	void modelLoaded(Model* model)
	{
		auto map_iter = m_model_entity_map.find(model);
		EntityPtr e = map_iter.value();
		while(e.isValid()) {
			modelLoaded(model, (EntityRef)e);
			e = m_model_instances[e.index].next_model;
		}
	}


	void allocateCustomMeshes(ModelInstance& r, int count)
	{
		if (hasCustomMeshes(r) && r.mesh_count == count) return;

		ASSERT(r.model);
		auto& rm = r.model->getResourceManager();
		auto* material_manager = static_cast<MaterialManager*>(rm.getOwner().get(Material::TYPE));

		auto* new_meshes = (Mesh*)m_allocator.allocate(count * sizeof(Mesh));
		if (r.meshes)
		{
			for (int i = 0; i < r.mesh_count; ++i)
			{
				new (NewPlaceholder(), new_meshes + i) Mesh(r.meshes[i]);
			}

			if (hasCustomMeshes(r))
			{
				for (int i = count; i < r.mesh_count; ++i)
				{
					material_manager->unload(*r.meshes[i].material);
				}
				for (int i = 0; i < r.mesh_count; ++i)
				{
					r.meshes[i].~Mesh();
				}
				m_allocator.deallocate(r.meshes);
			}
			else
			{
				for (int i = 0; i < r.mesh_count; ++i)
				{
					material_manager->load(*r.meshes[i].material);
					addToCustomMeshCallbacks(r.meshes[i].material, (EntityRef)r.entity);
				}
			}
		}

		for (int i = r.mesh_count; i < count; ++i)
		{
			ffr::VertexDecl decl;
			new (NewPlaceholder(), new_meshes + i) Mesh(nullptr, decl, "", nullptr, m_allocator);
		}
		r.meshes = new_meshes;
		r.mesh_count = count;
		r.flags.set(ModelInstance::CUSTOM_MESHES);
	}

	void removeFromCustomMeshCallbacks(Material* material, EntityRef entity)
	{
		auto iter = m_custom_mesh_callbacks.find(material);
		ASSERT(iter.isValid());
		auto& entities = iter.value().m_entities;
		for(int i = 0, c = entities.size(); i < c; ++i) {
			if (entities[i] == entity) {
				entities.eraseFast(i);
				break;
			}
		}
		if(entities.empty()) {
			m_custom_mesh_callbacks.erase(material);
		}
	}

	void addToCustomMeshCallbacks(Material* material, EntityRef entity)
	{
		auto iter = m_custom_mesh_callbacks.find(material);
		if(!iter.isValid()) {
			CustomMeshCallback cb(*this, material, m_allocator);
			m_custom_mesh_callbacks.insert(material, Move(cb));
			iter = m_custom_mesh_callbacks.find(material);
		}

		iter.value().m_entities.push(entity);
	}


	void setModelInstanceMaterial(EntityRef entity, int index, const Path& path) override
	{
		auto& r = m_model_instances[entity.index];
		const bool had_custom_meshes = hasCustomMeshes(r);
		if (r.meshes && r.mesh_count > index && r.meshes[index].material && path == r.meshes[index].material->getPath()) return;

		auto& rm = r.model->getResourceManager().getOwner();

		int new_count = Math::maximum(i8(index + 1), r.mesh_count);
		allocateCustomMeshes(r, new_count);
		ASSERT(r.meshes);

		Material* old_material = r.meshes[index].material;
		auto* new_material = rm.load<Material>(path);
		if(old_material) {
			removeFromCustomMeshCallbacks(old_material, entity);
		}

		r.meshes[index].setMaterial(new_material, *r.model, m_renderer);

		addToCustomMeshCallbacks(new_material, entity);
	}


	Path getModelInstanceMaterial(EntityRef entity, int index) override
	{
		auto& r = m_model_instances[entity.index];
		if (!r.meshes) return Path("");

		return r.meshes[index].material->getPath();
	}

	
	void addToModelEntityMap(Model* model, EntityRef entity)
	{
		ModelInstance& r = m_model_instances[entity.index];
		r.prev_model = INVALID_ENTITY;
		auto map_iter = m_model_entity_map.find(model);
		if(map_iter.isValid()) {
			r.next_model = map_iter.value();
			m_model_entity_map[model] = entity;
		}
		else {
			r.next_model = INVALID_ENTITY;
			m_model_entity_map.insert(model, entity);
			model->getObserverCb().bind<RenderSceneImpl, &RenderSceneImpl::modelStateChanged>(this);
		}
	}


	void removeFromModelEntityMap(Model* model, EntityRef entity)
	{
		ModelInstance& r = m_model_instances[entity.index];
		if(r.prev_model.isValid()) {
			m_model_instances[r.prev_model.index].next_model = r.next_model;
		}
		if(r.next_model.isValid()) {
			m_model_instances[r.next_model.index].prev_model = r.prev_model;
		}
		auto map_iter = m_model_entity_map.find(model);
		if(map_iter.value() == entity) {
			if(r.next_model.isValid()) {
				m_model_entity_map[model] = (EntityRef)r.next_model;
			}
			else {
				m_model_entity_map.erase(model);
				model->getObserverCb().unbind<RenderSceneImpl, &RenderSceneImpl::modelStateChanged>(this);
			}
		}
	}


	void setModel(EntityRef entity, Model* model)
	{
		auto& model_instance = m_model_instances[entity.index];
		ASSERT(model_instance.entity.isValid());
		Model* old_model = model_instance.model;
		bool no_change = model == old_model && old_model;
		if (no_change)
		{
			old_model->getResourceManager().unload(*old_model);
			return;
		}
		if (old_model)
		{
			auto& rm = old_model->getResourceManager();
			auto* material_manager = static_cast<MaterialManager*>(rm.getOwner().get(Material::TYPE));
			freeCustomMeshes(model_instance, material_manager);
			
			removeFromModelEntityMap(old_model, entity);

			if (old_model->isReady())
			{
				m_culling_system->remove(entity);
			}
			old_model->getResourceManager().unload(*old_model);
		}
		model_instance.model = model;
		model_instance.meshes = nullptr;
		model_instance.mesh_count = 0;
		LUMIX_DELETE(m_allocator, model_instance.pose);
		model_instance.pose = nullptr;
		if (model)
		{
			addToModelEntityMap(model, entity);

			if (model->isReady())
			{
				modelLoaded(model, entity);
			}
		}
	}

	IAllocator& getAllocator() override { return m_allocator; }


	float getLightFOV(EntityRef entity) override
	{
		return m_point_lights[m_point_lights_map[entity]].m_fov;
	}


	void setLightFOV(EntityRef entity, float fov) override
	{
		m_point_lights[m_point_lights_map[entity]].m_fov = fov;
	}


	void createGlobalLight(EntityRef entity)
	{
		GlobalLight light;
		light.m_entity = entity;
		light.m_diffuse_color.set(1, 1, 1);
		light.m_diffuse_intensity = 0;
		light.m_indirect_intensity = 1;
		light.m_fog_color.set(1, 1, 1);
		light.m_fog_density = 0;
		light.m_cascades.set(3, 8, 100, 300);
		light.m_fog_bottom = 0.0f;
		light.m_fog_height = 10.0f;

		if (m_global_lights.empty()) m_active_global_light_entity = entity;

		m_global_lights.insert(entity, light);
		m_universe.onComponentCreated(entity, GLOBAL_LIGHT_TYPE, this);
	}


	void createPointLight(EntityRef entity)
	{
		PointLight& light = m_point_lights.emplace();
		light.m_entity = entity;
		light.m_diffuse_color.set(1, 1, 1);
		light.m_diffuse_intensity = 1;
		light.m_fov = Math::degreesToRadians(360);
		light.m_specular_color.set(1, 1, 1);
		light.m_specular_intensity = 1;
		light.m_cast_shadows = false;
		light.m_attenuation_param = 2;
		light.m_range = 10;
		m_point_lights_map.insert(entity, m_point_lights.size() - 1);

		m_universe.onComponentCreated(entity, POINT_LIGHT_TYPE, this);
	}


	void updateDecalInfo(Decal& decal) const
	{
		/*decal.position = m_universe.getPosition(decal.entity);
		decal.radius = decal.scale.length();
		decal.mtx = m_universe.getMatrix(decal.entity);
		decal.mtx.setXVector(decal.mtx.getXVector() * decal.scale.x);
		decal.mtx.setYVector(decal.mtx.getYVector() * decal.scale.y);
		decal.mtx.setZVector(decal.mtx.getZVector() * decal.scale.z);
		decal.inv_mtx = decal.mtx;
		decal.inv_mtx.inverse();*/
		// TODO
		ASSERT(false);
	}


	void createDecal(EntityRef entity)
	{
		Decal& decal = m_decals.insert(entity);
		decal.material = nullptr;
		decal.entity = entity;
		decal.scale.set(1, 1, 1);
		updateDecalInfo(decal);

		m_universe.onComponentCreated(entity, DECAL_TYPE, this);
	}


	void createEnvironmentProbe(EntityRef entity)
	{
		EnvironmentProbe& probe = m_environment_probes.insert(entity);
		ResourceManagerHub& rm = m_engine.getResourceManager();
		probe.texture = rm.load<Texture>(Path("models/common/default_probe.dds"));
		probe.texture->setFlag(Texture::Flags::SRGB, true);
		probe.irradiance = rm.load<Texture>(Path("models/common/default_probe.dds"));
		probe.irradiance->setFlag(Texture::Flags::SRGB, true);
		probe.radiance = rm.load<Texture>(Path("models/common/default_probe.dds"));
		probe.radiance->setFlag(Texture::Flags::SRGB, true);
		probe.radius = 1;
		probe.flags.set(EnvironmentProbe::ENABLED);
		probe.guid = Math::randGUID();

		m_universe.onComponentCreated(entity, ENVIRONMENT_PROBE_TYPE, this);
	}


	void createBoneAttachment(EntityRef entity)
	{
		BoneAttachment& attachment = m_bone_attachments.emplace(entity);
		attachment.entity = entity;
		attachment.parent_entity = INVALID_ENTITY;
		attachment.bone_index = -1;

		m_universe.onComponentCreated(entity, BONE_ATTACHMENT_TYPE, this);
	}


	void createModelInstance(EntityRef entity)
	{
		while(entity.index >= m_model_instances.size())
		{
			auto& r = m_model_instances.emplace();
			r.entity = INVALID_ENTITY;
			r.model = nullptr;
			r.pose = nullptr;
		}
		auto& r = m_model_instances[entity.index];
		r.entity = entity;
		r.model = nullptr;
		r.meshes = nullptr;
		r.pose = nullptr;
		r.flags.clear();
		r.flags.set(ModelInstance::ENABLED);
		r.mesh_count = 0;
		m_universe.onComponentCreated(entity, MODEL_INSTANCE_TYPE, this);
	}


	void setParticleEmitterPath(EntityRef entity, const Path& path) override
	{
		if (!m_particle_emitters[entity]) return;

		ParticleEmitterResource* res = m_engine.getResourceManager().load<ParticleEmitterResource>(path);
		m_particle_emitters[entity]->setResource(res);
	}


	Path getParticleEmitterPath(EntityRef entity) override
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		if (!emitter) return Path("");
		if (!emitter->getResource()) return Path("");

		return emitter->getResource()->getPath();
	}


	const AssociativeArray<EntityRef, ParticleEmitter*>& getParticleEmitters() const override
	{
		return m_particle_emitters;
	}

private:
	IAllocator& m_allocator;
	Universe& m_universe;
	Renderer& m_renderer;
	Engine& m_engine;
	CullingSystem* m_culling_system;

	EntityPtr m_active_global_light_entity;
	HashMap<EntityRef, int> m_point_lights_map;

	AssociativeArray<EntityRef, Decal> m_decals;
	Array<ModelInstance> m_model_instances;
	HashMap<EntityRef, GlobalLight> m_global_lights;
	Array<PointLight> m_point_lights;
	HashMap<EntityRef, Camera> m_cameras;
	EntityPtr m_active_camera;
	AssociativeArray<EntityRef, TextMesh*> m_text_meshes;
	AssociativeArray<EntityRef, BoneAttachment> m_bone_attachments;
	AssociativeArray<EntityRef, EnvironmentProbe> m_environment_probes;
	HashMap<EntityRef, Terrain*> m_terrains;
	AssociativeArray<EntityRef, ParticleEmitter*> m_particle_emitters;

	Array<DebugTriangle> m_debug_triangles;
	Array<DebugLine> m_debug_lines;
	Array<DebugPoint> m_debug_points;

	float m_time;
	float m_lod_multiplier;
	bool m_is_updating_attachments;
	bool m_is_grass_enabled;
	bool m_is_game_running;

	HashMap<Model*, EntityRef> m_model_entity_map;
	HashMap<Material*, CustomMeshCallback> m_custom_mesh_callbacks;
};



#define COMPONENT_TYPE(type, name) \
	{ \
		type \
		, static_cast<Universe::Serialize>(&RenderSceneImpl::serialize##name) \
		, static_cast<Universe::Deserialize>(&RenderSceneImpl::deserialize##name) \
		, &RenderSceneImpl::create##name \
		, &RenderSceneImpl::destroy##name \
	}

static struct
{
	ComponentType type;
	Universe::Serialize serialize;
	Universe::Deserialize deserialize;
	void (RenderSceneImpl::*creator)(EntityRef);
	void (RenderSceneImpl::*destroyer)(EntityRef);
} COMPONENT_INFOS[] = {
	COMPONENT_TYPE(MODEL_INSTANCE_TYPE, ModelInstance),
	COMPONENT_TYPE(GLOBAL_LIGHT_TYPE, GlobalLight),
	COMPONENT_TYPE(POINT_LIGHT_TYPE, PointLight),
	COMPONENT_TYPE(DECAL_TYPE, Decal),
	COMPONENT_TYPE(CAMERA_TYPE, Camera),
	COMPONENT_TYPE(TERRAIN_TYPE, Terrain),
	COMPONENT_TYPE(BONE_ATTACHMENT_TYPE, BoneAttachment),
	COMPONENT_TYPE(ENVIRONMENT_PROBE_TYPE, EnvironmentProbe),
	COMPONENT_TYPE(PARTICLE_EMITTER_TYPE, ParticleEmitter),
	COMPONENT_TYPE(TEXT_MESH_TYPE, TextMesh)
};

#undef COMPONENT_TYPE

RenderSceneImpl::RenderSceneImpl(Renderer& renderer,
	Engine& engine,
	Universe& universe,
	IAllocator& allocator)
	: m_engine(engine)
	, m_universe(universe)
	, m_renderer(renderer)
	, m_allocator(allocator)
	, m_model_entity_map(m_allocator)
	, m_custom_mesh_callbacks(m_allocator)
	, m_model_instances(m_allocator)
	, m_cameras(m_allocator)
	, m_text_meshes(m_allocator)
	, m_terrains(m_allocator)
	, m_point_lights(m_allocator)
	, m_global_lights(m_allocator)
	, m_decals(m_allocator)
	, m_debug_triangles(m_allocator)
	, m_debug_lines(m_allocator)
	, m_debug_points(m_allocator)
	, m_active_global_light_entity(INVALID_ENTITY)
	, m_active_camera(INVALID_ENTITY)
	, m_is_grass_enabled(true)
	, m_is_game_running(false)
	, m_particle_emitters(m_allocator)
	, m_point_lights_map(m_allocator)
	, m_bone_attachments(m_allocator)
	, m_environment_probes(m_allocator)
	, m_lod_multiplier(1.0f)
	, m_time(0)
	, m_is_updating_attachments(false)
{
	m_universe.entityTransformed().bind<RenderSceneImpl, &RenderSceneImpl::onEntityMoved>(this);
	m_universe.entityDestroyed().bind<RenderSceneImpl, &RenderSceneImpl::onEntityDestroyed>(this);
	m_culling_system = CullingSystem::create(m_allocator);
	m_model_instances.reserve(5000);

	MaterialManager& manager = m_renderer.getMaterialManager();

	for (auto& i : COMPONENT_INFOS)
	{
		universe.registerComponentType(i.type, this, i.creator, i.destroyer, i.serialize, i.deserialize);
	}
}


RenderScene* RenderScene::createInstance(Renderer& renderer,
	Engine& engine,
	Universe& universe,
	IAllocator& allocator)
{
	return LUMIX_NEW(allocator, RenderSceneImpl)(renderer, engine, universe, allocator);
}


void RenderScene::destroyInstance(RenderScene* scene)
{
	LUMIX_DELETE(scene->getAllocator(), static_cast<RenderSceneImpl*>(scene));
}


void RenderScene::registerLuaAPI(lua_State* L)
{
	Model::registerLuaAPI(L);

	#define REGISTER_FUNCTION(F)\
		do { \
		auto f = &LuaWrapper::wrapMethod<RenderSceneImpl, decltype(&RenderSceneImpl::F), &RenderSceneImpl::F>; \
		LuaWrapper::createSystemFunction(L, "Renderer", #F, f); \
		} while(false) \

	REGISTER_FUNCTION(setGlobalLODMultiplier);
	REGISTER_FUNCTION(getGlobalLODMultiplier);
	REGISTER_FUNCTION(getCameraViewProjection);
	REGISTER_FUNCTION(getGlobalLightEntity);
	REGISTER_FUNCTION(getActiveGlobalLight);
	REGISTER_FUNCTION(getModelInstanceModel);
	REGISTER_FUNCTION(addDebugCross);
	REGISTER_FUNCTION(addDebugLine);
	REGISTER_FUNCTION(addDebugCircle);
	REGISTER_FUNCTION(addDebugSphere);
	REGISTER_FUNCTION(getTerrainMaterial);
	REGISTER_FUNCTION(getTerrainNormalAt);
	REGISTER_FUNCTION(setTerrainHeightAt);
	REGISTER_FUNCTION(enableModelInstance);
	REGISTER_FUNCTION(getPoseBonePosition);

	#undef REGISTER_FUNCTION

	#define REGISTER_FUNCTION(F)\
		do { \
		auto f = &LuaWrapper::wrap<decltype(&RenderSceneImpl::LUA_##F), &RenderSceneImpl::LUA_##F>; \
		LuaWrapper::createSystemFunction(L, "Renderer", #F, f); \
		} while(false) \

	REGISTER_FUNCTION(createPipeline);
	REGISTER_FUNCTION(destroyPipeline);
	REGISTER_FUNCTION(setPipelineScene);
	REGISTER_FUNCTION(getPipelineScene);
	// TODO
	/*REGISTER_FUNCTION(pipelineRender);
	REGISTER_FUNCTION(getRenderBuffer);*/
	REGISTER_FUNCTION(setModelInstanceMaterial);
	REGISTER_FUNCTION(setModelInstancePath);
	REGISTER_FUNCTION(getModelBoneIndex);
	REGISTER_FUNCTION(makeScreenshot);
	REGISTER_FUNCTION(compareTGA);
	REGISTER_FUNCTION(getTerrainHeightAt);

	LuaWrapper::createSystemFunction(L, "Renderer", "castCameraRay", &RenderSceneImpl::LUA_castCameraRay);

	#undef REGISTER_FUNCTION
}


} // namespace Lumix