#pragma once

#include "core/lux.h"
#include "core/array.h"
#include "core/string.h"
#include "engine/iplugin.h"
#include "graphics/ray_cast_model_hit.h"


namespace Lux
{


class Engine;
class IRenderDevice;
class Material;
class Model;
class ModelInstance;
class Pipeline;
class Pose;
class ResourceManager;
class Shader;
class Texture;
class Universe;


struct RenderableInfo
{
	ModelInstance* m_model_instance;
	float m_scale;
};


class LUX_ENGINE_API Renderer : public IPlugin 
{
	public:
		static Renderer* createInstance();
		static void destroyInstance(Renderer& renderer);

		virtual void render(IRenderDevice& device) = 0;
		virtual void renderGame() = 0;
		virtual void setUniverse(Universe* universe) = 0;
		virtual RayCastModelHit castRay(const Vec3& origin, const Vec3& dir) = 0;
		virtual void enableZTest(bool enable) = 0;
		virtual void setRenderDevice(IRenderDevice& device) = 0;

		virtual void applyCamera(Component camera) = 0;
		virtual void setCameraSize(Component camera, int w, int h) = 0;
		virtual void setCameraPriority(Component camera, const int& priority) = 0;
		virtual void getCameraPriority(Component camera, int& priority) = 0;
		virtual void getRay(Component camera, float x, float y, Vec3& origin, Vec3& dir) = 0;
		virtual Component getLight(int index) = 0;

		virtual Pose& getPose(Component cmp) = 0;
		virtual void setRenderableLayer(Component cmp, const int32_t& layer) = 0;
		virtual void setRenderablePath(Component cmp, const string& path) = 0;
		virtual void setRenderableScale(Component cmp, const float& scale) = 0;
		virtual void getRenderablePath(Component cmp, string& path) = 0;
		virtual void getRenderableInfos(Array<RenderableInfo>& infos, int64_t layer_mask) = 0;
		virtual void getCameraFov(Component cmp, float& fov) = 0;
		virtual Pipeline* loadPipeline(const char* path) = 0;
		virtual Engine& getEngine() = 0;
		
		/// "immediate mode"
		virtual void renderModel(const Model& model, const Matrix& transform) = 0;
		virtual Model* getModel(const char* path) = 0;
		/*		virtual void renderScene();
		void endFrame();
		void enableStage(const char* name, bool enable);
		int getWidth() const;
		int getHeight() const;
		void getRay(int x, int y, Vec3& origin, Vec3& dir);
		Component createRenderable(Entity entity);
		void destroyRenderable(Component cmp);
		Component createPointLight(Entity entity);
		void destroyPointLight(Component cmp);
		float getHalfFovTan();
		void setUniverse(Universe* universe);
		Component getRenderable(Universe& universe, H3DNode node);

		void getVisible(Component cmp, bool& visible);
		void setVisible(Component cmp, const bool& visible);
		void getMesh(Component cmp, string& str);
		void setMesh(Component cmp, const string& str);
		void getCastShadows(Component cmp, bool& cast_shadows);
		void setCastShadows(Component cmp, const bool& cast_shadows);
		bool getBonePosition(Component cmp, const char* bone_name, Vec3* out);

		H3DNode getMeshNode(Component cmp);
		void getLightFov(Component cmp, float& fov);
		void setLightFov(Component cmp, const float& fov);
		void getLightRadius(Component cmp, float& r);
		void setLightRadius(Component cmp, const float& r);
		H3DNode getRawCameraNode();
		void onResize(int w, int h);
		void getCameraMatrix(Matrix& mtx);
		void setCameraMatrix(const Matrix& mtx);
		const char* getBasePath() const;
		bool isReady() const;

		void serialize(ISerializer& serializer);
		void deserialize(ISerializer& serializer);*/
};


} // !namespace Lux

