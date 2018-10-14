#pragma once


#include "engine/array.h"
#include "engine/hash_map.h"
#include "engine/delegate_list.h"
#include "engine/path.h"
#include "engine/resource.h"
#include "engine/mt/sync.h"


namespace Lumix
{

namespace FS { struct IFile; }

class Material;
class WorldEditor;
struct Action;
class FileSystemWatcher;
class StudioApp;


class LUMIX_EDITOR_API AssetBrowser
{
public:
	struct LUMIX_EDITOR_API IPlugin
	{
		virtual ~IPlugin() {}

		virtual bool canCreateResource() const { return false; }
		virtual bool createResource(char* out_path, int max_size) { return false; }
		virtual void onGUI(Resource* resource) = 0;
		virtual void onResourceUnloaded(Resource* resource) = 0;
		virtual const char* getName() const = 0;
		virtual ResourceType getResourceType() const = 0;
		virtual bool createTile(const char* in_path, const char* out_path, ResourceType type);
		virtual void update() {}
	};

	typedef DelegateList<void(const Path&, const char*)> OnResourceChanged;

public:
	explicit AssetBrowser(StudioApp& app);
	~AssetBrowser();
	void onGUI();
	void update();
	int getTypeIndex(ResourceType type) const;
	void selectResource(const Path& resource, bool record_history);
	bool resourceInput(const char* label, const char* str_id, char* buf, int max_size, ResourceType type);
	void addPlugin(IPlugin& plugin);
	void removePlugin(IPlugin& plugin);
	void openInExternalEditor(Resource* resource) const;
	void openInExternalEditor(const char* path) const;
	void enableUpdate(bool enable) { m_is_update_enabled = enable; }
	bool resourceList(char* buf, int max_size, ResourceType type, float height) const;
	FS::IFile* beginSaveResource(Resource& resource);
	void endSaveResource(Resource& resource, FS::IFile& file, bool success);

public:
	bool m_is_open;
	float m_left_column_width = 120;
	static const int TILE_SIZE = 128;

private:
	struct FileInfo
	{
		StaticString<MAX_PATH_LENGTH> clamped_filename;
		StaticString<MAX_PATH_LENGTH> filepath;
		u32 file_path_hash;
		void* tex = nullptr;
		bool create_called = false;
	};

private:
	void dirColumn();
	void fileColumn();
	void detailsGUI();
	void createTile(FileInfo& tile, const char* out_path);
	void thumbnail(FileInfo& tile);
	int getThumbnailIndex(int i, int j, int columns) const;
	void doFilter();
	void breadcrumbs();
	void changeDir(const char* path);
	void onFileChanged(const char* path);
	void unloadResource();
	void selectResource(Resource* resource, bool record_history);
	void goBack();
	void goForward();
	void toggleAutoreload();
	bool isAutoreload() const { return m_autoreload_changed_resource; }


private:
	StudioApp& m_app;
	Array<Path> m_changed_files;
	StaticString<MAX_PATH_LENGTH> m_dir;
	Array<StaticString<MAX_PATH_LENGTH> > m_subdirs;
	Array<FileInfo> m_file_infos;
	Array<int> m_filtered_file_infos;
	Array<Path> m_history;
	int m_history_index;
	AssociativeArray<ResourceType, IPlugin*> m_plugins;
	MT::SpinMutex m_changed_files_mutex;
	Resource* m_selected_resource;
	WorldEditor& m_editor;
	int m_current_type;
	char m_filter[128];
	Path m_wanted_resource;
	bool m_autoreload_changed_resource;
	bool m_is_focus_requested;
	bool m_activate;
	bool m_is_update_enabled;
	bool m_show_thumbnails;
	Action* m_auto_reload_action;
	Action* m_back_action;
	Action* m_forward_action;
};


} // namespace Lumix