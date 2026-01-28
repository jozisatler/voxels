#include "VoxLoader.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <sstream>
#include <cmath>
#include <chrono>
#include <thread>
#include <future>

#include "geomVertexFormat.h"
#include "geomVertexData.h"
#include "geomVertexWriter.h"
#include "geomTriangles.h"
#include "geomNode.h"

struct MemoryReader {
    const char* start;
    const char* ptr;
    const char* end;
    
    MemoryReader(const std::vector<char>& buffer) {
        start = buffer.data();
        ptr = start;
        end = start + buffer.size();
    }
    
    template <typename T>
    bool read(T& val) {
        if (ptr + sizeof(T) > end) return false;
        std::memcpy(&val, ptr, sizeof(T));
        ptr += sizeof(T);
        return true;
    }
    
    bool read_bytes(void* dst, size_t size) {
        if (ptr + size > end) return false;
        std::memcpy(dst, ptr, size);
        ptr += size;
        return true;
    }
    
    size_t tell() const { return ptr - start; }
    void seek(size_t pos) { 
        if (pos <= (size_t)(end - start)) ptr = start + pos; 
    }
    bool good() const { return ptr < end; }
};

struct ChunkHeader {
    char id[4];
    int contentSize;
    int childrenSize;
};

bool match_id(const char* id, const char* target) {
    return strncmp(id, target, 4) == 0;
}

std::string read_vox_string(MemoryReader& f) {
    int n;
    if (!f.read(n)) return "";
    if (n < 0 || n > 10000) return ""; // sanity check
    std::string s(n, 0);
    f.read_bytes(&s[0], n);
    return s;
}

std::map<std::string, std::string> read_dict(MemoryReader& f) {
    std::map<std::string, std::string> dict;
    int n;
    if (!f.read(n)) return dict;
    for (int i = 0; i < n; ++i) {
        std::string k = read_vox_string(f);
        std::string v = read_vox_string(f);
        dict[k] = v;
    }
    return dict;
}

LMatrix3f decode_rotation(unsigned char r) {
    // ROW MAJOR in specification
    // bit | value
    // 0-1 : 1 : index of the non-zero entry in the first row
    // 2-3 : 2 : index of the non-zero entry in the second row
    // 4   : 0 : the sign in the first row (0 : positive; 1 : negative)
    // 5   : 1 : the sign in the second row (0 : positive; 1 : negative)
    // 6   : 1 : the sign in the third row (0 : positive; 1 : negative)
    
    int idx0 = (r & 3);
    int idx1 = (r >> 2) & 3;
    int idx2 = 3 - idx0 - idx1; // Since it's a permutation

    int sign0 = (r >> 4) & 1 ? -1 : 1;
    int sign1 = (r >> 5) & 1 ? -1 : 1;
    int sign2 = (r >> 6) & 1 ? -1 : 1;

    // Initialize with zeros manually
    LMatrix3f mat(0,0,0, 0,0,0, 0,0,0);
    mat(0, idx0) = (float)sign0;
    mat(1, idx1) = (float)sign1;
    mat(2, idx2) = (float)sign2;
    
    return mat;
}

void VoxelVolume::init_grid() {
    sizeX = data.sizeX;
    sizeY = data.sizeY;
    sizeZ = data.sizeZ;
    int count = sizeX * sizeY * sizeZ;
    grid.resize(count, 0);
    
    // Fill grid
    for (const auto& v : data.voxels) {
        int idx = v.x + v.y * sizeX + v.z * sizeX * sizeY;
        if (idx >= 0 && idx < count) {
            grid[idx] = v.colorIndex;
        }
    }
    
    // Initialize Chunks
    int nCx = (sizeX + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int nCy = (sizeY + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int nCz = (sizeZ + CHUNK_SIZE - 1) / CHUNK_SIZE;
    
    chunks.clear();
    chunks.resize(nCx * nCy * nCz);
    
    // Reset flags
    for(auto& c : chunks) c.hasVoxels = false;

    // Determine active chunks from data points directly (faster than iterating grid)
    for (const auto& v : data.voxels) {
        int cx = v.x / CHUNK_SIZE;
        int cy = v.y / CHUNK_SIZE;
        int cz = v.z / CHUNK_SIZE;
        int idx = cx + cy * nCx + cz * nCx * nCy;
        if (idx >= 0 && idx < chunks.size()) {
            chunks[idx].hasVoxels = true;
        }
    }
    
    for(int z=0; z<nCz; ++z) {
        for(int y=0; y<nCy; ++y) {
            for(int x=0; x<nCx; ++x) {
                int idx = x + y * nCx + z * nCx * nCy;
                chunks[idx].cx = x * CHUNK_SIZE;
                chunks[idx].cy = y * CHUNK_SIZE;
                chunks[idx].cz = z * CHUNK_SIZE;
            }
        }
    }
}



void VoxelVolume::update_mesh() {
    compute_mesh();
    apply_mesh();
}

void VoxelVolume::compute_mesh() {
    int nCx = (sizeX + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int strideZ = nCx * ((sizeY + CHUNK_SIZE - 1) / CHUNK_SIZE);
    
    // Process all active chunks
    for (auto& chunk : chunks) {
        if (!chunk.hasVoxels) {
            chunk.pendingBuffers = nullptr;
            continue;
        }
        
        int ex = std::min(sizeX, chunk.cx + CHUNK_SIZE);
        int ey = std::min(sizeY, chunk.cy + CHUNK_SIZE);
        int ez = std::min(sizeZ, chunk.cz + CHUNK_SIZE);

        // Generate buffers
        auto buffers = VoxLoader::generate_buffers_from_grid(grid, sizeX, sizeY, sizeZ, 
                                                               chunk.cx, chunk.cy, chunk.cz, 
                                                               ex, ey, ez, 
                                                               (LColor*)palette);
                                                               
        if (buffers && !buffers->interleavedData.empty()) {
             // Create GeomNode in parallel (Thread-safe in modern Panda3D)
             int num_verts = buffers->interleavedData.size() / 28;
             PT(GeomVertexData) vdata = new GeomVertexData("voxels", GeomVertexFormat::get_v3n3c4(), Geom::UH_static);
             vdata->unclean_set_num_rows(num_verts);
             
             PT(GeomVertexArrayDataHandle) handle = vdata->modify_array(0)->modify_handle();
             unsigned char* ptr = handle->get_write_pointer();
             std::memcpy(ptr, buffers->interleavedData.data(), buffers->interleavedData.size());
             
             PT(GeomTriangles) prim = new GeomTriangles(Geom::UH_static);
             int num_faces = num_verts / 4; 
             
             for (int i = 0; i < num_faces; ++i) {
                 int base = i * 4;
                 prim->add_vertices(base, base + 1, base + 2);
                 prim->add_vertices(base, base + 2, base + 3);
             }
             
             PT(Geom) geom = new Geom(vdata);
             geom->add_primitive(prim);
             PT(GeomNode) gnode = new GeomNode("vox_geom");
             gnode->add_geom(geom);
             
             chunk.pendingNode = gnode;
        } else {
             chunk.pendingNode = nullptr;
        }
        
        chunk.pendingBuffers = nullptr; // Not needed anymore
    }
}

void VoxelVolume::apply_mesh() {
    for (auto& chunk : chunks) {
         // handle voxel removal
         if (!chunk.hasVoxels) {
             if (!chunk.node.is_empty()) chunk.node.remove_node();
             continue;
         }
         
         if (chunk.pendingNode) {
             if (!chunk.node.is_empty()) chunk.node.remove_node();
             
             NodePath chunkNode = nodePath.attach_new_node(chunk.pendingNode);
             chunk.node = chunkNode;
             
             chunk.pendingNode = nullptr; // Cleanup
         } else if (chunk.hasVoxels && chunk.node.is_empty()) {
             // Should not happen if compute_mesh ran correctly, unless chunk is empty geometry (all internal/invisible)
         }
    }
}

void VoxelVolume::update_region(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
    // Fallback to updating specific chunks immediately (on main thread usually)
    // or we can reuse compute/apply for specific indices.
    // Ideally this function is called during gameplay for editing.
    
    if (chunks.empty()) return;
    if (sizeX == 0 || sizeY == 0 || sizeZ == 0) return;
    
    int nCx = (sizeX + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int strideZ = nCx * ((sizeY + CHUNK_SIZE - 1) / CHUNK_SIZE);
    
    int cMinX = minX / CHUNK_SIZE;
    int cMinY = minY / CHUNK_SIZE;
    int cMinZ = minZ / CHUNK_SIZE;
    
    int cMaxX = (maxX - 1) / CHUNK_SIZE;
    int cMaxY = (maxY - 1) / CHUNK_SIZE;
    int cMaxZ = (maxZ - 1) / CHUNK_SIZE;
    
    for (int z = cMinZ; z <= cMaxZ; ++z) {
        for (int y = cMinY; y <= cMaxY; ++y) {
            for (int x = cMinX; x <= cMaxX; ++x) {
                int cIdx = x + y * nCx + z * strideZ;
                if (cIdx < 0 || cIdx >= chunks.size()) continue;
                
                Chunk& chunk = chunks[cIdx];
                
                // Assuming modifications updated grid, we need to re-check if it has voxels? 
                // For simplified editing, we always re-gen active region or check scanning.
                // But for now, just regen.
                
                int ex = std::min(sizeX, chunk.cx + CHUNK_SIZE);
                int ey = std::min(sizeY, chunk.cy + CHUNK_SIZE);
                int ez = std::min(sizeZ, chunk.cz + CHUNK_SIZE);
                
                PT(GeomNode) gnode = VoxLoader::generate_mesh_from_grid(grid, sizeX, sizeY, sizeZ, 
                                                                        chunk.cx, chunk.cy, chunk.cz, 
                                                                        ex, ey, ez, 
                                                                        (LColor*)palette);
                                                                        
                if (!chunk.node.is_empty()) chunk.node.remove_node();
                
                if (gnode) {
                     chunk.node = nodePath.attach_new_node(gnode);
                     chunk.node.set_collide_mask(CollideMask::all_on());
                     chunk.hasVoxels = true;
                } else {
                    chunk.hasVoxels = false;
                }
            }
        }
    }
}

bool VoxelVolume::destroy_at(const LPoint3& localPos, float radius) {
    if (grid.empty()) return false;

    // Use grid logic
    float cx = sizeX / 2.0f;
    float cy = sizeY / 2.0f;
    float cz = sizeZ / 2.0f;
    
    // Local pos is centered, so add center to get index
    float vx = localPos.get_x() + cx;
    float vy = localPos.get_y() + cy;
    float vz = localPos.get_z() + cz;
    
    int centerX = (int)std::round(vx);
    int centerY = (int)std::round(vy);
    int centerZ = (int)std::round(vz);
    
    int r = (int)std::ceil(radius);
    int r2 = (int)(radius * radius);
    
    bool changed = false;
    
    // Optimized loop over bounding box
    int minX = std::max(0, centerX - r);
    int maxX = std::min(sizeX - 1, centerX + r);
    int minY = std::max(0, centerY - r);
    int maxY = std::min(sizeY - 1, centerY + r);
    int minZ = std::max(0, centerZ - r);
    int maxZ = std::min(sizeZ - 1, centerZ + r);
    
    for (int z = minZ; z <= maxZ; ++z) {
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                float dx = x - vx;
                float dy = y - vy;
                float dz = z - vz;
                 
                if (dx*dx + dy*dy + dz*dz <= r2) {
                    int idx = x + y * sizeX + z * sizeX * sizeY;
                    if (grid[idx] != 0) {
                        grid[idx] = 0;
                        changed = true;
                    }
                }
            }
        }
    }

    if (changed) {
        // Expand bounds by 1 for mesh processing to handle neighbor faces properly
        update_region(minX - 1, minY - 1, minZ - 1, maxX + 2, maxY + 2, maxZ + 2);
    }
    return changed;
}

void VoxLoader::load_default_palette(LColor* palette) {
    for (int i=0; i<256; ++i) {
        palette[i].set(i/255.0f, i/255.0f, i/255.0f, 1.0f);
    }
}

NodePath VoxLoader::load_vox(const std::string& filename, WindowFramework* window, std::vector<VoxelVolume*>& outVolumes, std::function<void(float, std::string)> progressCallback) {
    if (progressCallback) progressCallback(0.0f, "Opening file...");
    std::cout << "Loading VOX file: " << filename << std::endl;
    auto start_total = std::chrono::high_resolution_clock::now();

    // Read entire file to memory
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filename << std::endl;
        return NodePath("loader_error");
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
         std::cerr << "Failed to read " << filename << std::endl;
         return NodePath("loader_error");
    }
    file.close();

    if (progressCallback) progressCallback(0.1f, "Parsing VOX structure...");

    MemoryReader reader(buffer);

    char magic[4];
    int version;
    reader.read_bytes(magic, 4);
    reader.read(version);
    
    if (!match_id(magic, "VOX ")) return NodePath("loader_error");

    std::vector<VoxData> models;
    LColor palette[256];
    VoxLoader::load_default_palette(palette);
    
    std::map<int, SceneNode*> sceneNodes;

    ChunkHeader header;
    if (reader.read(header)) {
       if (match_id(header.id, "MAIN")) {
           long endOfMain = (long)reader.tell() + header.childrenSize;
           
           while (reader.tell() < endOfMain && reader.good()) {
               ChunkHeader child;
               if (!reader.read(child)) break;
               
               long nextChunkPos = (long)reader.tell() + child.contentSize + child.childrenSize;

               if (match_id(child.id, "SIZE")) {
                   VoxData data;
                   reader.read(data.sizeX);
                   reader.read(data.sizeY);
                   reader.read(data.sizeZ);
                   models.push_back(data);
               } 
               else if (match_id(child.id, "XYZI")) {
                    if (models.empty()) { VoxData d; models.push_back(d); }
                    int numVoxels;
                    reader.read(numVoxels);
                    VoxData& current = models.back();
                    current.voxels.resize(numVoxels);
                    reader.read_bytes(current.voxels.data(), numVoxels * 4);
               }
               else if (match_id(child.id, "RGBA")) {
                   unsigned char buf[1024];
                   reader.read_bytes(buf, 1024);
                   for (int i = 0; i < 255; ++i) {
                       int idx = i * 4;
                       palette[i + 1].set(buf[idx]/255.0f, buf[idx+1]/255.0f, buf[idx+2]/255.0f, buf[idx+3]/255.0f);
                   }
               }
               else if (match_id(child.id, "nTRN")) {
                   TransformNode* node = new TransformNode();
                   reader.read(node->id);
                   read_dict(reader); // node attributes
                   reader.read(node->childId);
                   int reserved; reader.read(reserved);
                   reader.read(node->layerId);
                   int num_frames; reader.read(num_frames);
                   
                   // Only read first frame
                   if (num_frames > 0) {
                       auto dict = read_dict(reader);
                       if (dict.count("_t")) {
                           std::stringstream ss(dict["_t"]);
                           float x, y, z;
                           ss >> x >> y >> z;
                           node->translation.set(x, y, z);
                           node->hasTranslation = true;
                       }
                       if (dict.count("_r")) {
                           unsigned char r = (unsigned char)std::stoi(dict["_r"]);
                           node->rotation = decode_rotation(r);
                           node->hasRotation = true;
                       }
                   }
                   // Skip other frames
                   for (int i=1; i<num_frames; ++i) read_dict(reader);
                   sceneNodes[node->id] = node;
               }
               else if (match_id(child.id, "nGRP")) {
                   GroupNode* node = new GroupNode();
                   reader.read(node->id);
                   read_dict(reader);
                   int num_children; reader.read(num_children);
                   for (int i=0; i<num_children; ++i) {
                       int cid; reader.read(cid);
                       node->childrenIds.push_back(cid);
                   }
                   sceneNodes[node->id] = node;
               }
               else if (match_id(child.id, "nSHP")) {
                   ShapeNode* node = new ShapeNode();
                   reader.read(node->id);
                   read_dict(reader);
                   int num_models; reader.read(num_models);
                   for (int i=0; i<num_models; ++i) {
                       int mid; reader.read(mid);
                       node->modelIds.push_back(mid);
                       read_dict(reader); // model attributes
                   }
                   sceneNodes[node->id] = node;
               }
               
               reader.seek(nextChunkPos);
           }
       }
    }
    
    auto end_parse = std::chrono::high_resolution_clock::now();
    std::cout << "Parsing file took: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_parse - start_total).count() << "ms" << std::endl;
    std::cout << "Parsing file took: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_parse - start_total).count() << "ms" << std::endl;
    std::cout << "Parsing done. Models: " << models.size() << " Nodes: " << sceneNodes.size() << std::endl;

    if (progressCallback) progressCallback(0.2f, "Building Scene Graph...");

    NodePath root("VoxScene");
    
    if (sceneNodes.empty()) {
        // Fallback for simple structures without scene graph
        for (size_t i = 0; i < models.size(); ++i) {
             VoxelVolume* vol = new VoxelVolume();
             vol->data = models[i];
             std::memcpy(vol->palette, palette, sizeof(palette));
             vol->init_grid(); // Initialize grid
             // Defer mesh gen
             outVolumes.push_back(vol);
        }
    } else {
        // Find root node? Usually ID 0 is root.
        if (sceneNodes.find(0) != sceneNodes.end()) {
            build_scene_graph(0, sceneNodes, models, palette, root, outVolumes);
        } else {
            std::cerr << "Root node 0 not found!" << std::endl;
        }
        
        // Cleanup nodes
        for (auto& pair : sceneNodes) {
             delete pair.second;
        }
    }
    
    // Parallel Mesh Generation
    auto t_mesh_start = std::chrono::high_resolution_clock::now();
    
    // std::cout << "Starting parallel mesh gen for " << outVolumes.size() << " volumes." << std::endl;
    
    // Use futures to run compute_mesh in parallel
    std::vector<std::future<void>> jobs;
    int total_vols = outVolumes.size();
    std::atomic<int> completed_vols(0);
    
    for (size_t i = 0; i < outVolumes.size(); ++i) {
        VoxelVolume* vol = outVolumes[i];
        jobs.push_back(std::async(std::launch::async, [vol, i, total_vols, &completed_vols, progressCallback](){
            try {
                vol->compute_mesh();
                int c = ++completed_vols;
                if (progressCallback && (c % 5 == 0 || c == total_vols)) {
                     // Note: Calling callback from thread might be unsafe if it touches GPU/Panda global state without locks.
                     // But we will handle thread safety in main.cpp or just update atomic/mutex there.
                     // For simplicity, we assume the callback handles synchronization or just prints.
                     // IMPORTANT: TextNode update is not thread safe.
                }
            } catch (const std::exception& e) {
                std::cerr << "Exception in vol " << i << ": " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception in vol " << i << std::endl;
            }
        }));
    }
    
    // Wait and update progress in main thread
    for (int i=0; i<jobs.size(); ++i) {
        if (progressCallback) {
             float p = 0.3f + 0.6f * ((float)i / jobs.size());
             progressCallback(p, "Generating Meshes " + std::to_string(i) + "/" + std::to_string(jobs.size()));
        }
        jobs[i].get(); 
    }
    
    auto t_mesh_comp_end = std::chrono::high_resolution_clock::now();
    
    if (progressCallback) progressCallback(0.9f, "Uploading Meshes to GPU...");
    
    // Apply (sequential on main thread)
    std::cout << "Mesh comp done. Applying..." << std::endl;
    for (auto* vol : outVolumes) {
        vol->apply_mesh();
    }
    
    auto t_mesh_end = std::chrono::high_resolution_clock::now();
    
    std::cout << "Mesh computation took: " << std::chrono::duration_cast<std::chrono::milliseconds>(t_mesh_comp_end - t_mesh_start).count() << "ms" << std::endl;
    std::cout << "Mesh update total took: " << std::chrono::duration_cast<std::chrono::milliseconds>(t_mesh_end - t_mesh_start).count() << "ms" << std::endl;
    
    auto end_total = std::chrono::high_resolution_clock::now();
    std::cout << "Total load_vox time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_total - start_total).count() << "ms" << std::endl;
    
    return root;
}

void VoxLoader::build_scene_graph(int nodeId, 
                                 const std::map<int, SceneNode*>& nodes, 
                                 const std::vector<VoxData>& models,
                                 const LColor* palette,
                                 NodePath& parent,
                                 std::vector<VoxelVolume*>& outVolumes) {
    if (nodes.find(nodeId) == nodes.end()) return;
    
    SceneNode* node = nodes.at(nodeId);
    
    if (TransformNode* trn = dynamic_cast<TransformNode*>(node)) {
        NodePath np = parent.attach_new_node("TRN_" + std::to_string(nodeId));
        
        if (trn->hasRotation) {
            LMatrix4f rotMat = LMatrix4f::ident_mat();
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                     // Transposed for Panda3D
                    rotMat(j, i) = trn->rotation(i, j);
                }
            }
            np.set_mat(rotMat);
        }
        
        if (trn->hasTranslation) {
            np.set_pos(trn->translation);
        }
        
        build_scene_graph(trn->childId, nodes, models, palette, np, outVolumes);
    }
    else if (GroupNode* grp = dynamic_cast<GroupNode*>(node)) {
        NodePath np = parent.attach_new_node("GRP_" + std::to_string(nodeId));
        for (int childId : grp->childrenIds) {
            build_scene_graph(childId, nodes, models, palette, np, outVolumes);
        }
    }
    else if (ShapeNode* shp = dynamic_cast<ShapeNode*>(node)) {
        NodePath np = parent.attach_new_node("SHP_" + std::to_string(nodeId));
        for (int modelId : shp->modelIds) {
            if (modelId >= 0 && modelId < models.size()) {
                // Instantiating a new VoxelVolume for this shape
                VoxelVolume* vol = new VoxelVolume();
                vol->data = models[modelId]; // Copy data!
                std::memcpy(vol->palette, palette, sizeof(vol->palette));
                vol->init_grid(); // Initialize grid
                
                vol->nodePath = np.attach_new_node("Volume_Model" + std::to_string(modelId));
                
                // Deferred mesh update
                
                outVolumes.push_back(vol);
            }
        }
    }
}

    struct FaceInfo {
        unsigned short x, y, z;
        unsigned char side; 
        unsigned char cIdx;
    };

    // Pure C++ generation with Basic Greedy Meshing (X and Y axis merge)
    std::shared_ptr<MeshBuffers> VoxLoader::generate_buffers_from_grid(const std::vector<unsigned char>& grid, int sx, int sy, int sz, 
                                            int minX, int minY, int minZ, int maxX, int maxY, int maxZ,
                                            const LColor* palette) {
        // ... (offsets and verts same as before)
        static const int neighbor_offsets[6][3] = {
            {0,0,-1}, {0,0,1}, {0,-1,0}, {1,0,0}, {0,1,0}, {-1,0,0}
        };
        // Quad vertices for each face type (relative to 0,0,0)
        static const float face_verts[6][4][3] = {
             {{0,1,0}, {1,1,0}, {1,0,0}, {0,0,0}}, // -Z
             {{0,0,1}, {1,0,1}, {1,1,1}, {0,1,1}}, // +Z
             {{0,0,0}, {1,0,0}, {1,0,1}, {0,0,1}}, // -Y
             {{1,0,0}, {1,1,0}, {1,1,1}, {1,0,1}}, // +X
             {{1,1,0}, {0,1,0}, {0,1,1}, {1,1,1}}, // +Y
             {{0,1,0}, {0,0,0}, {0,0,1}, {0,1,1}}  // -X
        };
        static const float face_normals[6][3] = {
            {0,0,-1}, {0,0,1}, {0,-1,0}, {1,0,0}, {0,1,0}, {-1,0,0}
        };
        
        auto result = std::make_shared<MeshBuffers>();
        // Estimate size: 4000 verts * 28 bytes
        result->interleavedData.reserve(4000 * 28); 

        int strideY = sx;
        int strideZ = sx * sy;
        
        float cx = sx / 2.0f;
        float cy = sy / 2.0f;
        float cz = sz / 2.0f;
        
        // Track visited faces to avoid re-meshing merged ones
        // We need a bitmask or array. bitmask is smaller.
        // Size: sx * sy * sz * 6 bits?
        // Or just clear current slice.
        // Since we sweep z, then y, then x, we can just use a visited array for the current slices?
        // Actually full greedy meshing requires tracking visited faces.
        // For simplicity: We implement X-merging (already done). Y-merging requires knowing if row below is same.
        // Let's stick to X-merging for now but verify if we can do better.
        // Wait, the previous X-merge code didn't use a visited array because it skipped processed voxels by advancing `x += w`.
        // To do 2D merging (X and Y), we need a visited array for the current plane.
        
        std::vector<bool> visited(sx * sy); // For current Z slice and current Face direction

        // Iterate over faces direction
        for (int f=0; f<6; ++f) {
            int nx = neighbor_offsets[f][0];
            int ny = neighbor_offsets[f][1];
            int nz = neighbor_offsets[f][2];
            
            // Sweep Z (independent planes for faces 0,1. For side faces, Z changes plane content).
            // Actually, for faces 0(-Z) and 1(+Z), the plane is XY.
            // For faces 2(-Y) and 4(+Y), the plane is XZ.
            // For faces 3(+X) and 5(-X), the plane is YZ.
            // But we iterate x,y,z loops.
            // Let's simplify: Just do 1D merge on X for now as implemented, but check if we can do 2D.
            // If we use visited array, we can do 2D.
            // Let's implement full 2D greedy meshing for current Z-slice (conceptually).
            
            // Since we loop Z outer, we are effectively slicing the volume.
            // Faces 0 and 1 are perfect for this (normals along Z).
            // Faces 2,3,4,5 are perpendicular to slice.
            // Only faces 0 and 1 can be merged in 2D (X and Y) easily with Z-sweep.
            // Side faces can be merged in X (if f=2,4) or Y (if f=3,5) and Z (if we change loop order).
            // Changing loop order per face is complex.
            // Let's just stick to 1D X-merge for all faces.
            // But wait, for faces 3(+X) and 5(-X), X is the normal. Merging along X is invalid (not coplanar).
            // We should merge along Y or Z for those.
            // My previous code disabled merging for 3 and 5.
            
            // Let's improve:
            
            for (int z=minZ; z<maxZ; ++z) {
                for (int y=minY; y<maxY; ++y) {
                    for (int x=minX; x<maxX; ++x) {
                        int idx = x + y * strideY + z * strideZ;
                        unsigned char c = grid[idx];
                        if (c == 0) continue;
                        
                        // Check visibility
                        int bx = x + nx;
                        int by = y + ny;
                        int bz = z + nz;
                        bool visible = true;
                        if (bx >= 0 && bx < sx && by >= 0 && by < sy && bz >= 0 && bz < sz) {
                            if (grid[bx + by * strideY + bz * strideZ] != 0) visible = false;
                        }
                        if (!visible) continue;
                        
                        // Greedy Merge
                        int w = 1;
                        int h = 1; // For 2D merge later
                        
                        // X-Merge for faces 0,1,2,4 (Not 3,5)
                        if (f != 3 && f != 5) {
                             while (x + w < maxX) {
                                 int next_x = x + w;
                                 int next_idx = idx + w;
                                 if (grid[next_idx] != c) break;
                                 // Check visibility
                                 int nbx = next_x + nx;
                                 int nby = y + ny;
                                 int nbz = z + nz;
                                 bool next_visible = true;
                                 if (nbx >= 0 && nbx < sx && nby >= 0 && nby < sy && nbz >= 0 && nbz < sz) {
                                     if (grid[nbx + nby * strideY + nbz * strideZ] != 0) next_visible = false;
                                 }
                                 if (!next_visible) break;
                                 w++;
                             }
                        }
                        // Y-Merge for faces 3,5 (Not 0,1,2,4 to simplify conflicts? Or can we?)
                        // For face 3(+X), plane is YZ. Merging along Y is valid.
                        if (f == 3 || f == 5) {
                             while (y + w < maxY) { // Reusing 'w' as 'height' in Y direction for this case
                                 int next_y = y + w;
                                 int next_idx = idx + w * strideY;
                                 if (grid[next_idx] != c) break;
                                 // Check visibility
                                 int nbx = x + nx;
                                 int nby = next_y + ny;
                                 int nbz = z + nz;
                                 bool next_visible = true;
                                 if (nbx >= 0 && nbx < sx && nby >= 0 && nby < sy && nbz >= 0 && nbz < sz) {
                                     if (grid[nbx + nby * strideY + nbz * strideZ] != 0) next_visible = false;
                                 }
                                 if (!next_visible) break;
                                 w++;
                             }
                        }

                        // Output Quad
                        LColor col = palette[c];
                        float vx = x - cx;
                        float vy = y - cy;
                        float vz = z - cz;
                        
                        // Apply stretch
                        for (int i = 0; i < 4; ++i) {
                            float lx = face_verts[f][i][0];
                            float ly = face_verts[f][i][1];
                            float lz = face_verts[f][i][2];
                            
                            // 0,1,2,4: X-stretch
                            if (f != 3 && f != 5) {
                                lx *= w;
                            } else {
                                // 3,5: Y-stretch
                                ly *= w;
                            }
                            
                            float fp[6];
                            fp[0] = vx + lx;
                            fp[1] = vy + ly;
                            fp[2] = vz + lz;
                            fp[3] = face_normals[f][0];
                            fp[4] = face_normals[f][1];
                            fp[5] = face_normals[f][2];
                            
                            unsigned char cp[4];
                            cp[0] = (unsigned char)(col[0] * 255.0f);
                            cp[1] = (unsigned char)(col[1] * 255.0f);
                            cp[2] = (unsigned char)(col[2] * 255.0f);
                            cp[3] = (unsigned char)(col[3] * 255.0f);
                            
                            // Push raw bytes
                            size_t curSize = result->interleavedData.size();
                            result->interleavedData.resize(curSize + 28);
                            unsigned char* ptr = result->interleavedData.data() + curSize;
                            std::memcpy(ptr, fp, 24); // 6 floats * 4 bytes
                            std::memcpy(ptr + 24, cp, 4); // 4 bytes
                        }
                        
                        if (f != 3 && f != 5) {
                            x += (w - 1);
                        } else {
                           // No Y merge for now in X-loop
                        }
                    }
                }
            }
        }
        return result;
    }

    // Optimized generic wrapper for old code (not used in parallel path)
    PT(GeomNode) VoxLoader::generate_mesh_from_grid(const std::vector<unsigned char>& grid, int sx, int sy, int sz, 
                                            int minX, int minY, int minZ, int maxX, int maxY, int maxZ,
                                            const LColor* palette) {
        // Use buffer generator then convert immediately
        auto buffers = generate_buffers_from_grid(grid, sx, sy, sz, minX, minY, minZ, maxX, maxY, maxZ, palette);
        if (!buffers || buffers->interleavedData.empty()) return nullptr;
        
        int num_verts = buffers->interleavedData.size() / 28;
        PT(GeomVertexData) vdata = new GeomVertexData("voxels", GeomVertexFormat::get_v3n3c4(), Geom::UH_static);
        vdata->unclean_set_num_rows(num_verts);
        
        PT(GeomVertexArrayDataHandle) handle = vdata->modify_array(0)->modify_handle();
        unsigned char* ptr = handle->get_write_pointer();
        std::memcpy(ptr, buffers->interleavedData.data(), buffers->interleavedData.size());
        
        PT(GeomTriangles) prim = new GeomTriangles(Geom::UH_static);
        int num_faces = num_verts / 4;
        for (int i = 0; i < num_faces; ++i) {
            int base = i * 4;
            prim->add_vertices(base, base + 1, base + 2);
            prim->add_vertices(base, base + 2, base + 3);
        }
        
        PT(Geom) geom = new Geom(vdata);
        geom->add_primitive(prim);
        PT(GeomNode) node = new GeomNode("vox_geom");
        node->add_geom(geom);
        return node;
    }
    
    // Kept for compatibility but not used by VoxelVolume anymore optimization
    void VoxLoader::generate_mesh(const VoxData& data, const LColor* palette, NodePath& parent) {
       // ... simplified wrapper or just keep old code?
       // Let's implement it using our new function to save space/duplication
       std::vector<unsigned char> grid(data.sizeX * data.sizeY * data.sizeZ, 0);
       for (const auto& v : data.voxels) {
            int idx = v.x + v.y * data.sizeX + v.z * data.sizeX * data.sizeY;
            if (idx >= 0 && idx < grid.size()) grid[idx] = v.colorIndex;
       }
       PT(GeomNode) node = generate_mesh_from_grid(grid, data.sizeX, data.sizeY, data.sizeZ, 0, 0, 0, data.sizeX, data.sizeY, data.sizeZ, palette);
       if(node) parent.attach_new_node(node);
    }
