#include "pandaFramework.h"
#include "pandaSystem.h"
#include "genericAsyncTask.h"
#include "asyncTaskManager.h"
#include "VoxLoader.h"

#include "collisionTraverser.h"
#include "collisionHandlerQueue.h"
#include "collisionRay.h"
#include "collisionNode.h"
#include "mouseWatcher.h"
#include "keyboardButton.h"
#include "mouseButton.h"
#include "windowProperties.h"
#include "directionalLight.h"
#include "ambientLight.h"

// Global state for interaction
struct GameState {
    PandaFramework* framework;
    WindowFramework* window;
    std::vector<VoxelVolume*> volumes;
    float destructionRadius = 1.5f; // Default radius
    
    // Camera control
    float speed = 50.0f;
    float sensitivity = 0.1f;
    float pitch = 0.0f;
    float heading = 0.0f;
    
    // Collision
    CollisionTraverser* traverser = nullptr;
    PT(CollisionHandlerQueue) queue;
    PT(CollisionRay) pickerRay;
};

// Task for Free Look Camera (WASD + Mouse)
// Task for Free Look Camera (WASD + Mouse)
AsyncTask::DoneStatus cameraTask(GenericAsyncTask* task, void* data) {
    GameState* state = static_cast<GameState*>(data);
    if (!state || !state->window || !state->window->get_graphics_window()) return AsyncTask::DS_cont;
    
    NodePath camera = state->window->get_camera_group();
    NodePath mouseNode = state->window->get_mouse();
    
    if (mouseNode.is_empty()) return AsyncTask::DS_cont;
    MouseWatcher* mouseWatcher = DCAST(MouseWatcher, mouseNode.node());
    if (!mouseWatcher) return AsyncTask::DS_cont;
    
    ClockObject* globalClock = ClockObject::get_global_clock();
    double dt = globalClock ? globalClock->get_dt() : 0.016;
    
    // Keyboard Movement
    LVector3 move(0, 0, 0);
    if (mouseWatcher->is_button_down(KeyboardButton::ascii_key('w'))) move.set_y(1);
    if (mouseWatcher->is_button_down(KeyboardButton::ascii_key('s'))) move.set_y(-1);
    if (mouseWatcher->is_button_down(KeyboardButton::ascii_key('a'))) move.set_x(-1);
    if (mouseWatcher->is_button_down(KeyboardButton::ascii_key('d'))) move.set_x(1); // Typo fixed from 'a'
    
    if (move.length_squared() > 0) move.normalize();
    if (move.length_squared() > 0) move.normalize();
    camera.set_pos(camera, move * state->speed * dt);
    
    static int frameCount = 0;
    frameCount++;
    if (frameCount % 100 == 0) {
        LPoint3 pos = camera.get_pos();
        LVecBase3 hpr = camera.get_hpr();
        std::cout << "Cam Pos: " << pos.get_x() << ", " << pos.get_y() << ", " << pos.get_z() 
                  << " HPR: " << hpr.get_x() << ", " << hpr.get_y() << ", " << hpr.get_z() << std::endl;
    }
    
    // Mouse Rotation
    GraphicsWindow* gw = state->window->get_graphics_window();
    if (gw && mouseWatcher->has_mouse()) {
         if (gw->get_num_input_devices() > 0) {
             try {
                 MouseData md = gw->get_pointer(0);
                 int currentX = md.get_x();
                 int currentY = md.get_y();
                 
                 WindowProperties props = gw->get_properties();
                 int centerX = props.get_x_size() / 2;
                 int centerY = props.get_y_size() / 2;
                 
                 int dx = currentX - centerX;
                 int dy = currentY - centerY;
                 
                 if (dx != 0 || dy != 0) {
                     state->heading -= dx * state->sensitivity;
                     state->pitch -= dy * state->sensitivity;
                     
                     if (state->pitch > 90) state->pitch = 90;
                     if (state->pitch < -90) state->pitch = -90;
                     
                     camera.set_hpr(state->heading, state->pitch, 0);
                     
                     
                     // Re-center mouse
                     gw->move_pointer(0, centerX, centerY);
                 }
             } catch (...) {
             }
         }
    }
    
    return AsyncTask::DS_cont;
}

// Interaction Task (Scroll + Click)
AsyncTask::DoneStatus interactionTask(GenericAsyncTask* task, void* data) {
    GameState* state = static_cast<GameState*>(data);
    WindowFramework* window = state->window;
    
    NodePath mouseNode = window->get_mouse();
    if (mouseNode.is_empty()) return AsyncTask::DS_cont;
    
    MouseWatcher* mouseWatcher = DCAST(MouseWatcher, mouseNode.node());
    if (!mouseWatcher) return AsyncTask::DS_cont;
    
    // Check if picker needs setup (should be done in main, but safe check)
    if (!state->traverser) return AsyncTask::DS_cont; // Should start logic
    
    // Handle destruction on click
    
    // Handle destruction on click
    if (mouseWatcher->is_button_down(MouseButton::one())) { // Left Click
        // Set ray from camera center (crosshair)
        state->pickerRay->set_origin(0, 0, 0);
        state->pickerRay->set_direction(0, 1, 0); // Forward in camera space
        
        // std::cout << "Traversing..." << std::endl;
        state->traverser->traverse(window->get_render());
        
        if (state->queue->get_num_entries() > 0) {
            state->queue->sort_entries();
            PT(CollisionEntry) hitEntry = nullptr; 
            
            // Find closest hit that is a voxel volume
            for (int i=0; i<state->queue->get_num_entries(); ++i) {
                 PT(CollisionEntry) entry = state->queue->get_entry(i);
                 NodePath hitNode = entry->get_into_node_path();
                 NodePath parent = hitNode.get_parent();
                 
                 // Check if it belongs to any volume
                 bool found = false;
                 for (auto* volume : state->volumes) {
                     if (volume->nodePath == parent || volume->nodePath == hitNode) {
                         hitEntry = entry;
                         found = true;
                         break;
                     }
                 }
                 if (found) break;
            }
            
            if (hitEntry) {
                NodePath hitNode = hitEntry->get_into_node_path();
                LPoint3 hitPoint = hitEntry->get_surface_point(hitNode);
                LVector3 hitNormal = hitEntry->get_surface_normal(hitNode);
                
                // Nudge point slightly inside the voxel to ensure we hit the right one
                // Since normal points OUT, we subtract a bit of the normal
                LPoint3 targetPoint = hitPoint - (hitNormal * 0.5f);
                
                NodePath parent = hitNode.get_parent();

                for (auto* volume : state->volumes) {
                     if (volume->nodePath == parent || volume->nodePath == hitNode) {
                          if (volume->destroy_at(targetPoint, state->destructionRadius)) {
                               // std::cout << "Destroyed block at " << targetPoint << " radius " << state->destructionRadius << std::endl;
                          }
                          break;
                     }
                }
            }
        }
    }
    
    // Check for Scroll (Zoom) to change radius
    // Panda3D maps scroll to buttons? checking default mapping...
    // Usually MouseButton::wheel_up()
    
    // This task runs every frame, we need event driven for scroll usually, or check button state.
    // Button state for wheel is momentary.
    
    return AsyncTask::DS_cont;
}

// Event handlers
void scroll_up(const Event* event, void* data) {
    GameState* state = (GameState*)data;
    state->destructionRadius += 0.5f;
    std::cout << "Radius: " << state->destructionRadius << std::endl;
}

void scroll_down(const Event* event, void* data) {
    GameState* state = (GameState*)data;
    state->destructionRadius -= 0.5f;
    if (state->destructionRadius < 0.5f) state->destructionRadius = 0.5f;
    std::cout << "Radius: " << state->destructionRadius << std::endl;
}


int main(int argc, char* argv[]) {
    PandaFramework framework;
    framework.open_framework(argc, argv);
    framework.set_window_title("Destructible Castle Demo");
    
    WindowFramework* window = framework.open_window();
    if (window == nullptr) return 1;
    
    // Setup state
    GameState state;
    state.framework = &framework;
    state.window = window;
    
    // Initialize Collision System in State
    state.traverser = new CollisionTraverser("traverser");
    state.queue = new CollisionHandlerQueue();
    state.pickerRay = new CollisionRay();
    
    PT(CollisionNode) cNode = new CollisionNode("picker");
    cNode->add_solid(state.pickerRay);
    cNode->set_from_collide_mask(CollideMask::all_on());
    NodePath pickerNode = window->get_camera_group().attach_new_node(cNode);
    state.traverser->add_collider(pickerNode, state.queue);
    
    // Enable Keyboard
    window->enable_keyboard();
    
    // Lock mouse for Free Look
    // Lock mouse for Free Look
    WindowProperties props = window->get_graphics_window()->get_properties();
    props.set_cursor_hidden(true);
    // props.set_mouse_mode(WindowProperties::M_relative); // Let's keep absolute + manual re-center for now
    window->get_graphics_window()->request_properties(props);

    // Load Model
    NodePath root = VoxLoader::load_vox("castle.vox", window, state.volumes);
    
    if (!root.is_empty()) {
        root.reparent_to(window->get_render());
        root.set_scale(0.1);
        root.set_pos(0, 0, 0);
        
        // Lighting Setup
        // Ambient Light
        PT(AmbientLight) alight = new AmbientLight("alight");
        alight->set_color(LColor(0.3, 0.3, 0.3, 1));
        NodePath alnp = window->get_render().attach_new_node(alight);
        window->get_render().set_light(alnp);
        
        // Directional Light with Shadows
        PT(DirectionalLight) dlight = new DirectionalLight("dlight");
        dlight->set_color(LColor(0.8, 0.8, 0.8, 1));
        dlight->set_shadow_caster(true, 2048, 2048); 
        
        // Adjust lens for shadow coverage
        // Since the castle (root) is scaled to 0.1, we need to estimate the size.
        // Assuming castle is reasonably large, we might need a decent film size.
        // Let's try to fit it.
        Lens* lens = dlight->get_lens();
        lens->set_film_size(100, 100); 
        lens->set_near_far(10, 300);
        
        NodePath dlnp = window->get_render().attach_new_node(dlight);
        dlnp.set_pos(0, -100, 100); // Put it somewhere high and back
        dlnp.look_at(0, 0, 0);
        window->get_render().set_light(dlnp);
        
        // Enable Auto Shader for lighting and shadows
        window->get_render().set_shader_auto();
        
        // Position camera
        NodePath camera = window->get_camera_group();
        camera.set_pos(0, -150, 50); 
        camera.look_at(0, 0, 0);
        
        // Update state heading/pitch to match look_at
        state.heading = camera.get_h();
        state.pitch = camera.get_p();
        
        std::cout << "Loaded " << state.volumes.size() << " destructible volumes." << std::endl;
    }
    
    std::cout << "Adding camera task..." << std::endl;
    // Add Tasks
    GenericAsyncTask* camTask = new GenericAsyncTask("cameraTask", &cameraTask, &state);
    AsyncTaskManager::get_global_ptr()->add(camTask);
    
    std::cout << "Adding interaction task..." << std::endl;
    std::cout << "Adding interaction task..." << std::endl;
    GenericAsyncTask* intTask = new GenericAsyncTask("interactionTask", &interactionTask, &state);
    AsyncTaskManager::get_global_ptr()->add(intTask);
    
    std::cout << "Defining keys..." << std::endl;
    // Add Events for Scroll
    framework.define_key("wheel_up", "Validation", scroll_up, &state);
    framework.define_key("wheel_down", "Validation", scroll_down, &state);
    framework.define_key("escape", "Quit", [](const Event*, void*) { exit(0); }, nullptr);

    std::cout << "Starting main loop..." << std::endl;
    framework.main_loop();
    framework.close_framework();
    return 0;
}
