#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional> // Added for callback
#include "pandaFramework.h"
#include "pandaSystem.h"

struct Voxel {
    unsigned char x, y, z, colorIndex;
};

struct VoxData {
    int sizeX, sizeY, sizeZ;
    std::vector<Voxel> voxels;
};

// Scene Graph Nodes
struct SceneNode {
    int id;
    virtual ~SceneNode() = default;
};

struct TransformNode : public SceneNode {
    int childId;
    int layerId;
    // We only support the first frame for now
    LVecBase3f translation; // from _t
    // Rotation not fully implemented yet, defaulting to identity
    LMatrix3f rotation; // from _r
    bool hasTranslation;
    bool hasRotation;
    
    TransformNode() : translation(0,0,0), hasTranslation(false), hasRotation(false) {
        rotation = LMatrix3f::ident_mat();
    }
};

struct GroupNode : public SceneNode {
    std::vector<int> childrenIds;
};

struct ShapeNode : public SceneNode {
    std::vector<int> modelIds; // indices into VoxData models
};

struct MeshBuffers {
    // Interleaved data: V3 (12 bytes) + N3 (12 bytes) + C4 (4 bytes)
    // Total 28 bytes per vertex
    std::vector<unsigned char> interleavedData; 
    std::vector<int> indices; 
};

// Manages a single voxel model that can be modified
class VoxelVolume {
public:
    // Optimized grid storage
    std::vector<unsigned char> grid; // flattend 3D array: x + y*sx + z*sx*sy
    int sizeX, sizeY, sizeZ;

    VoxData data; // Keep for reference or remove? Let's keep for now for initial load, but grid is authority.
    NodePath nodePath; // The node containing the geometry
    LColor palette[256];



    // Chunking for performance
    static const int CHUNK_SIZE = 64;
    struct Chunk {
        int cx, cy, cz; // Start offsets
        NodePath node;
        bool hasVoxels = false;
        std::shared_ptr<MeshBuffers> pendingBuffers;
        PT(GeomNode) pendingNode;
    };
    std::vector<Chunk> chunks;

    VoxelVolume() : sizeX(0), sizeY(0), sizeZ(0) {}
    
    // updates the mesh based on current data (updates all chunks)
    void update_mesh();
    
    // Parallel-friendly split
    void compute_mesh();
    void apply_mesh();
    
    // Update specific region of chunks
    void update_region(int minX, int minY, int minZ, int maxX, int maxY, int maxZ);
    
    // Remove voxels in a radius around a local point
    // Returns true if anything changed
    bool destroy_at(const LPoint3& localPos, float radius);
    
    // Initialize grid from data
    void init_grid();
};

class VoxLoader {
public:
    // Loads the file and populates the volumes list with all the interactive models found
    static NodePath load_vox(const std::string& filename, WindowFramework* window, std::vector<VoxelVolume*>& outVolumes, std::function<void(float, std::string)> progressCallback = nullptr);
    
    // Helper to regenerate mesh (exposed for VoxelVolume)
    static void generate_mesh(const VoxData& data, const LColor* palette, NodePath& parent);
    
    // Pure C++ generation (Thread Safe)
    static std::shared_ptr<MeshBuffers> generate_buffers_from_grid(const std::vector<unsigned char>& grid, int sx, int sy, int sz, 
                                        int minX, int minY, int minZ, int maxX, int maxY, int maxZ,
                                        const LColor* palette);
                                        
    // Replaced/Deprecated
    static PT(GeomNode) generate_mesh_from_grid(const std::vector<unsigned char>& grid, int sx, int sy, int sz, 
                                        int minX, int minY, int minZ, int maxX, int maxY, int maxZ,
                                        const LColor* palette);
    static void load_default_palette(LColor* palette);

private:
    // Recursive scene graph builder
    static void build_scene_graph(int nodeId, const std::map<int, SceneNode*>& nodes, 
                                 const std::vector<VoxData>& models,
                                 const LColor* palette,
                                 NodePath& parent,
                                 std::vector<VoxelVolume*>& outVolumes);
};
