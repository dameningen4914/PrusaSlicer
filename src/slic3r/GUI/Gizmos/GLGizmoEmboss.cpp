#include "GLGizmoEmboss.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI_ObjectManipulation.hpp"
#include "slic3r/GUI/MainFrame.hpp" // to update title when add text
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/Jobs/EmbossJob.hpp"
#include "slic3r/GUI/Jobs/CreateFontNameImageJob.hpp"
#include "slic3r/GUI/Jobs/NotificationProgressIndicator.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

// TODO: remove include
#include "libslic3r/SVG.hpp"      // debug store
#include "libslic3r/Geometry.hpp" // covex hull 2d
#include "libslic3r/Timer.hpp" // covex hull 2d

#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp" // union_ex
#include "libslic3r/AppConfig.hpp"    // store/load font list
#include "libslic3r/Format/OBJ.hpp" // load obj file for default object
#include "libslic3r/BuildVolume.hpp"

#include "imgui/imgui_stdlib.h" // using std::string for inputs
#include "nanosvg/nanosvg.h"    // load SVG file

#include <wx/font.h>
#include <wx/fontutil.h>
#include <wx/fontdlg.h>
#include <wx/fontenum.h>

#include <boost/log/trivial.hpp>

#include <GL/glew.h>
#include <chrono> // measure enumeration of fonts

// uncomment for easier debug
//#define ALLOW_DEBUG_MODE
#ifdef ALLOW_DEBUG_MODE
#define ALLOW_ADD_FONT_BY_FILE
#define ALLOW_ADD_FONT_BY_OS_SELECTOR
#define SHOW_WX_FONT_DESCRIPTOR // OS specific descriptor | file path --> in edit style <tree header>
#define SHOW_FONT_FILE_PROPERTY // ascent, descent, line gap, cache --> in advanced <tree header>
#define SHOW_FONT_COUNT // count of enumerated font --> in font combo box
#define SHOW_CONTAIN_3MF_FIX // when contain fix matrix --> show gray '3mf' next to close button
#define SHOW_OFFSET_DURING_DRAGGING // when drag with text over surface visualize used center
#define SHOW_IMGUI_ATLAS
#define SHOW_ICONS_TEXTURE
#define SHOW_FINE_POSITION // draw convex hull around volume
#define SHOW_WX_WEIGHT_INPUT
#define DRAW_PLACE_TO_ADD_TEXT
#endif // ALLOW_DEBUG_MODE

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Emboss;

// anonymous namespace for unique names
namespace {
template<typename T>
struct MinMax
{
    T min;
    T max;
};
template<typename T>
struct Limit
{
    MinMax<T> gui;
    MinMax<T> values;
};
struct Limits
{
    MinMax<float> emboss{0.01f, 1e4f};
    MinMax<float> size_in_mm{0.1f, 1000.f};
    Limit<float> boldness{{-200.f, 200.f}, {-2e4f, 2e4f}};
    Limit<float> skew{{-1.f, 1.f}, {-100.f, 100.f}};
    MinMax<int>  char_gap{-20000, 20000};
    MinMax<int>  line_gap{-20000, 20000};
    // distance text object from surface
    MinMax<float> angle{-180.f, 180.f}; // in mm

    template<typename T>
    static bool apply(std::optional<T> &val, const MinMax<T> &limit) {
        if (val.has_value())
            return apply<T>(*val, limit);
        return false;
    }
    template<typename T>
    static bool apply(T &val, const MinMax<T> &limit)
    {
        if (val > limit.max) {
            val = limit.max;
            return true;
        }
        if (val < limit.min) {
            val = limit.min;
            return true;
        }
        return false;
    }
};
static const Limits limits;

static bool is_text_empty(const std::string &text){
    return text.empty() ||
           text.find_first_not_of(" \n\t\r") == std::string::npos;
}

// Normalize radian angle from -PI to PI
template<typename T> void to_range_pi_pi(T& angle)
{
    if (angle > PI || angle < -PI) {
        int count = static_cast<int>(std::round(angle / (2 * PI)));
        angle -= static_cast<T>(count * 2 * PI);
    }
}

} // namespace

GLGizmoEmboss::GLGizmoEmboss(GLCanvas3D &parent)
    : GLGizmoBase(parent, M_ICON_FILENAME, -2)
    , m_volume(nullptr)
    , m_is_unknown_font(false)
    , m_rotate_gizmo(parent, GLGizmoRotate::Axis::Z) // grab id = 2 (Z axis)
    , m_style_manager(m_imgui->get_glyph_ranges())
    , m_update_job_cancel(nullptr)
{
    m_rotate_gizmo.set_group_id(0);
    m_rotate_gizmo.set_using_local_coordinate(true);
    // TODO: add suggestion to use https://fontawesome.com/
    // (copy & paste) unicode symbols from web    
    // paste HEX unicode into notepad move cursor after unicode press [alt] + [x]
}

// Private namespace with helper function for create volume
namespace priv {

/// <summary>
/// Prepare data for emboss
/// </summary>
/// <param name="text">Text to emboss</param>
/// <param name="style_manager">Keep actual selected style</param>
/// <returns>Base data for emboss text</returns>
static DataBase create_emboss_data_base(const std::string &text, StyleManager &style_manager);

static bool is_valid(ModelVolumeType volume_type);

/// <summary>
/// Start job for add new volume to object with given transformation
/// </summary>
/// <param name="object">Define where to add</param>
/// <param name="volume_trmat">Volume transformation</param>
/// <param name="emboss_data">Define text</param>
/// <param name="volume_type">Type of volume</param>
static void start_create_volume_job(const ModelObject *object,
                                    const Transform3d  volume_trmat,
                                    DataBase          &emboss_data,
                                    ModelVolumeType    volume_type);

static GLVolume *get_hovered_gl_volume(const GLCanvas3D &canvas);

/// <summary>
/// Start job for add new volume on surface of object defined by screen coor
/// </summary>
/// <param name="emboss_data">Define params of text</param>
/// <param name="volume_type">Emboss / engrave</param>
/// <param name="screen_coor">Mouse position which define position</param>
/// <param name="gl_volume">Volume to find surface for create</param>
/// <param name="raycaster">Ability to ray cast to model</param>
/// <returns>True when start creation, False when there is no hit surface by screen coor</returns>
static bool start_create_volume_on_surface_job(DataBase         &emboss_data,
                                               ModelVolumeType   volume_type,
                                               const Vec2d      &screen_coor,
                                               const GLVolume   *gl_volume,
                                               RaycastManager   &raycaster);

/// <summary>
/// Find volume in selected object with closest convex hull to screen center.
/// Return 
/// </summary>
/// <param name="selection">Define where to search for closest</param>
/// <param name="screen_center">Canvas center(dependent on camera settings)</param>
/// <param name="objects">Actual objects</param>
/// <param name="closest_center">OUT: coordinate of controid of closest volume</param>
/// <param name="closest_volume">OUT: closest volume</param>
static void find_closest_volume(const Selection       &selection,
                                const Vec2d           &screen_center,
                                const Camera          &camera,
                                const ModelObjectPtrs &objects,
                                Vec2d                 *closest_center,
                                const GLVolume       **closest_volume);

/// <summary>
/// Start job for add object with text into scene
/// </summary>
/// <param name="emboss_data">Define params of text</param>
/// <param name="coor">Screen coordinat, where to create new object laying on bed</param>
static void start_create_object_job(DataBase &emboss_data, const Vec2d &coor);

/// <summary>
/// Create transformation for new created emboss object by mouse position
/// </summary>
/// <param name="screen_coor">Define where to add object</param>
/// <param name="camera">Actual camera view</param>
/// <param name="bed_shape">Define shape of bed for its center and check that coor is on bed center</param>
/// <param name="z">Emboss size / 2</param>
/// <returns>Transformation for create text on bed</returns>
static Transform3d create_transformation_on_bed(const Vec2d              &screen_coor,
                                                const Camera             &camera,
                                                const std::vector<Vec2d> &bed_shape,
                                                double                    z);
} // namespace priv

bool priv::is_valid(ModelVolumeType volume_type){
    if (volume_type == ModelVolumeType::MODEL_PART ||
        volume_type == ModelVolumeType::NEGATIVE_VOLUME ||
        volume_type == ModelVolumeType::PARAMETER_MODIFIER)
        return true;

    BOOST_LOG_TRIVIAL(error) << "Can't create embossed volume with this type: " << (int)volume_type;
    return false;
}

void GLGizmoEmboss::create_volume(ModelVolumeType volume_type, const Vec2d& mouse_pos)
{
    if (!priv::is_valid(volume_type)) return;
    if (!m_gui_cfg.has_value()) initialize();
    set_default_text();
    m_style_manager.discard_style_changes();
    
    GLVolume *gl_volume = priv::get_hovered_gl_volume(m_parent);
    DataBase emboss_data = priv::create_emboss_data_base(m_text, m_style_manager);
    // Try to cast ray into scene and find object for add volume 
    if (priv::start_create_volume_on_surface_job(emboss_data, volume_type, mouse_pos, gl_volume, m_raycast_manager))
        // object found
        return;

    // object is not under mouse position soo create object on plater
    priv::start_create_object_job(emboss_data, mouse_pos);
}

void GLGizmoEmboss::create_volume(ModelVolumeType volume_type)
{
    if (!priv::is_valid(volume_type)) return;
    if (!m_gui_cfg.has_value()) initialize();
    set_default_text();
    m_style_manager.discard_style_changes();

    // select position by camera position and view direction
    const Selection &selection = m_parent.get_selection();
    int object_idx = selection.get_object_idx();

    Size s = m_parent.get_canvas_size();
    Vec2d screen_center(s.get_width() / 2., s.get_height() / 2.);
    DataBase emboss_data = priv::create_emboss_data_base(m_text, m_style_manager); 
    if (!selection.is_empty() && object_idx >= 0) {
        // create volume inside of object
        const Plater &plater = *wxGetApp().plater();
        const Camera &camera = plater.get_camera();
        const ModelObjectPtrs &objects = wxGetApp().model().objects;

        Vec2d coor;
        const GLVolume *vol = nullptr;
        priv::find_closest_volume(selection, screen_center, camera, objects, &coor, &vol);
        if (!priv::start_create_volume_on_surface_job(emboss_data, volume_type, coor, vol, m_raycast_manager)) {
            assert(vol != nullptr);
            // in centroid of convex hull is not hit with object
            // soo create transfomation on border of object
            const ModelObject *obj = objects[vol->object_idx()];
            const BoundingBoxf3& bb = obj->bounding_box();
            Transform3d volume_trmat(Eigen::Translation3d(bb.max.x(), 0., 0.));
            priv::start_create_volume_job(obj, volume_trmat, emboss_data, volume_type);
        }
    } else {
        // create Object on center of screen
        // when ray throw center of screen not hit bed it create object on center of bed        
        priv::start_create_object_job(emboss_data, screen_center);
    }
}

bool GLGizmoEmboss::on_mouse_for_rotation(const wxMouseEvent &mouse_event)
{
    if (mouse_event.Moving()) return false;

    bool used = use_grabbers(mouse_event);
    if (!m_dragging) return used;

    if (mouse_event.Dragging()) {
        auto &angle_opt = m_volume->text_configuration->style.prop.angle;
        if (!m_rotate_start_angle.has_value())
            m_rotate_start_angle = angle_opt.has_value() ? *angle_opt : 0.f;        
        double angle = m_rotate_gizmo.get_angle();
        angle -= PI / 2; // Grabber is upward

        // temporary rotation
        TransformationType transformation_type = TransformationType::Local_Relative_Joint;
        m_parent.get_selection().rotate(Vec3d(0., 0., angle), transformation_type);

        angle += *m_rotate_start_angle;
        // move to range <-M_PI, M_PI>
        to_range_pi_pi(angle);
        // propagate angle into property
        angle_opt = static_cast<float>(angle);

        // do not store zero
        if (is_approx(*angle_opt, 0.f))
            angle_opt.reset();        

        // set into activ style
        assert(m_style_manager.is_activ_font());
        if (m_style_manager.is_activ_font())
            m_style_manager.get_font_prop().angle = angle_opt;

    }
    return used;
}

namespace priv {

/// <summary>
/// Access to model from gl_volume
/// TODO: it is more general function --> move to utils
/// </summary>
/// <param name="gl_volume">Volume to model belongs to</param>
/// <param name="object">Object containing gl_volume</param>
/// <returns>Model for volume</returns>
static ModelVolume *get_model_volume(const GLVolume *gl_volume, const ModelObject *object);

/// <summary>
/// Access to model from gl_volume
/// TODO: it is more general function --> move to utils
/// </summary>
/// <param name="gl_volume">Volume to model belongs to</param>
/// <param name="objects">All objects</param>
/// <returns>Model for volume</returns>
static ModelVolume *get_model_volume(const GLVolume *gl_volume, const ModelObjectPtrs &objects);

/// <summary>
/// Access to model by selection
/// TODO: it is more general function --> move to select utils
/// </summary>
/// <param name="selection">Actual selection</param>
/// <returns>Model from selection</returns>
static ModelVolume *get_selected_volume(const Selection &selection);

/// <summary>
/// Calculate offset from mouse position to center of text
/// </summary>
/// <param name="mouse">Screan mouse position</param>
/// <param name="mv">Selected volume(text)</param>
/// <returns>Offset in screan coordinate</returns>
static Vec2d calc_mouse_to_center_text_offset(const Vec2d &mouse, const ModelVolume &mv);

} // namespace priv

Vec2d priv::calc_mouse_to_center_text_offset(const Vec2d& mouse, const ModelVolume& mv) {
    const Transform3d &volume_tr   = mv.get_matrix();
    const Camera      &camera      = wxGetApp().plater()->get_camera();
    assert(mv.text_configuration.has_value());

    auto calc_offset = [&mouse, &volume_tr, &camera, &mv]
    (const Transform3d &instrance_tr) -> Vec2d {
        Transform3d to_world = instrance_tr * volume_tr;  

        // Use fix of .3mf loaded tranformation when exist
        if (mv.text_configuration->fix_3mf_tr.has_value())
            to_world = to_world * (*mv.text_configuration->fix_3mf_tr);        
        // zero point of volume in world coordinate system
        Vec3d volume_center = to_world.translation();
        // screen coordinate of volume center
        Vec2i coor = CameraUtils::project(camera, volume_center);
        return coor.cast<double>() - mouse;
    };

    auto object = mv.get_object();
    assert(!object->instances.empty());
    // Speed up for one instance
    if (object->instances.size() == 1) 
        return calc_offset(object->instances.front()->get_matrix());

    Vec2d  nearest_offset;
    double nearest_offset_size = std::numeric_limits<double>::max();
    for (const ModelInstance *instance : object->instances) {        
        Vec2d offset = calc_offset(instance->get_matrix());
        double offset_size = offset.norm();
        if (nearest_offset_size < offset_size) continue;
        nearest_offset_size = offset_size;
        nearest_offset      = offset;
    }
    return nearest_offset;
}

bool GLGizmoEmboss::on_mouse_for_translate(const wxMouseEvent &mouse_event)
{
    // filter events
    if (!(mouse_event.Dragging() && mouse_event.LeftIsDown()) && 
        !mouse_event.LeftUp() &&
        !mouse_event.LeftDown())
        return false;

    // must exist hover object
    int hovered_id = m_parent.get_first_hover_volume_idx();
    if (hovered_id < 0) return false;

    GLVolume *gl_volume = m_parent.get_volumes().volumes[hovered_id];
    const ModelObjectPtrs &objects = wxGetApp().plater()->model().objects;
    ModelVolume *act_model_volume = priv::get_model_volume(gl_volume, objects);

    // hovered object must be actual text volume
    if (m_volume != act_model_volume) return false;

    const ModelVolumePtrs &volumes = m_volume->get_object()->volumes;
    std::vector<size_t> allowed_volumes_id;
    if (volumes.size() > 1) {
        allowed_volumes_id.reserve(volumes.size() - 1);
        for (auto &v : volumes) { 
            if (v->id() == m_volume->id()) continue;
            if (!v->is_model_part()) continue;
            allowed_volumes_id.emplace_back(v->id().id);
        }
    }

    // wxCoord == int --> wx/types.h
    Vec2i mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d mouse_pos = mouse_coord.cast<double>();        
    RaycastManager::AllowVolumes condition(std::move(allowed_volumes_id));

    // detect start text dragging
    if (mouse_event.LeftDown()) {
        // initialize raycasters
        // IMPROVE: move to job, for big scene it slows down 
        ModelObject *act_model_object = act_model_volume->get_object();
        m_raycast_manager.actualize(act_model_object, &condition);
        m_dragging_mouse_offset = priv::calc_mouse_to_center_text_offset(mouse_pos, *m_volume);
        // Cancel job to prevent interuption of dragging (duplicit result)
        if (m_update_job_cancel != nullptr) m_update_job_cancel->store(true);
        return false;
    }

    // Dragging starts out of window
    if (!m_dragging_mouse_offset.has_value()) return false;

    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec2d offseted_mouse = mouse_pos + *m_dragging_mouse_offset;
    auto hit = m_raycast_manager.unproject(offseted_mouse, camera, &condition);
    if (!hit.has_value()) { 
        // there is no hit
        // show common translation of object
        m_parent.toggle_model_objects_visibility(true);
        m_temp_transformation = {};
        return false; 
    }
        
    if (mouse_event.Dragging()) {
        TextConfiguration &tc = *m_volume->text_configuration;
        // hide common dragging of object
        m_parent.toggle_model_objects_visibility(false, m_volume->get_object(), gl_volume->instance_idx(), m_volume);

        // Calculate temporary position
        Transform3d object_trmat = m_raycast_manager.get_transformation(hit->tr_key);
        Transform3d trmat = create_transformation_onto_surface(hit->position, hit->normal);
        const FontProp& font_prop = tc.style.prop;
        apply_transformation(font_prop, trmat);

        // fix baked transformation from .3mf store process
        if (tc.fix_3mf_tr.has_value())
            trmat = trmat * (*tc.fix_3mf_tr);

        // temp is in wolrld coors
        m_temp_transformation = object_trmat * trmat;
    } else if (mouse_event.LeftUp()) {
        // Added because of weird case after double click into scene 
        // with Mesa driver OR on Linux
        if (!m_temp_transformation.has_value()) return false;

        // TODO: Disable applying of common transformation after draggig
        // Call after is used for apply transformation after common dragging to rewrite it
        Transform3d volume_trmat =
            gl_volume->get_instance_transformation().get_matrix().inverse() *
            *m_temp_transformation;
        wxGetApp().plater()->CallAfter([volume_trmat, mv = m_volume]() {
            mv->set_transformation(volume_trmat);
        });

        m_parent.toggle_model_objects_visibility(true);
        // Apply temporary position
        m_temp_transformation = {};
        m_dragging_mouse_offset = {};

        // Update surface by new position
        if (m_volume->text_configuration->style.prop.use_surface) {
            // need actual position
            m_volume->set_transformation(volume_trmat);
            process();
        }
    }
    return false;
}

bool GLGizmoEmboss::on_mouse(const wxMouseEvent &mouse_event)
{
    // do not process moving event
    if (mouse_event.Moving()) return false;

    // not selected volume
    assert(m_volume != nullptr);
    assert(m_volume->text_configuration.has_value());
    if (m_volume == nullptr || !m_volume->text_configuration.has_value()) return false;

    if (on_mouse_for_rotation(mouse_event)) return true;
    if (on_mouse_for_translate(mouse_event)) return true;

    return false;
}

bool GLGizmoEmboss::on_init()
{
    m_rotate_gizmo.init();
    ColorRGBA gray_color(.6f, .6f, .6f, .3f);
    m_rotate_gizmo.set_highlight_color(gray_color);
    return true;
}

std::string GLGizmoEmboss::on_get_name() const { return _u8L("Emboss"); }

void GLGizmoEmboss::on_render() {
    // no volume selected
    if (m_volume == nullptr) return;
    Selection &selection = m_parent.get_selection();
    if (selection.is_empty()) return;

    if (m_temp_transformation.has_value()) {
        // draw text volume on temporary position
#if ENABLE_LEGACY_OPENGL_REMOVAL
        GLVolume& gl_volume = *selection.get_volume(*selection.get_volume_idxs().begin());
#else
        const GLVolume& gl_volume = *selection.get_volume(*selection.get_volume_idxs().begin());
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
#if ENABLE_LEGACY_OPENGL_REMOVAL
        GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
#else
        glsafe(::glPushMatrix());
        glsafe(::glMultMatrixd(m_temp_transformation->data()));                
        GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
        shader->start_using();

#if ENABLE_LEGACY_OPENGL_REMOVAL
        const Camera& camera = wxGetApp().plater()->get_camera();
        const Transform3d matrix = camera.get_view_matrix() * (*m_temp_transformation);
        shader->set_uniform("view_model_matrix", matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_normal_matrix", (Matrix3d) (matrix).matrix().block(0, 0, 3, 3).inverse().transpose());
        shader->set_uniform("emission_factor", 0.0f);
#endif // ENABLE_LEGACY_OPENGL_REMOVAL

        // dragging object must be selected so draw it with correct color
        //auto color = gl_volume.color;
        //auto color = gl_volume.render_color;
        auto color = GLVolume::SELECTED_COLOR;
        // Set transparent color for NEGATIVE_VOLUME & PARAMETER_MODIFIER
        bool is_transparent = m_volume->type() != ModelVolumeType::MODEL_PART;        
        if (is_transparent) {
            color.a(0.5f);
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        }
#if !ENABLE_LEGACY_OPENGL_REMOVAL
        shader->set_uniform("uniform_color", color);
#endif // !ENABLE_LEGACY_OPENGL_REMOVAL

        glsafe(::glEnable(GL_DEPTH_TEST));
#if ENABLE_LEGACY_OPENGL_REMOVAL
        gl_volume.model.set_color(color);
        gl_volume.model.render();
#else
        gl_volume.indexed_vertex_array.render();
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
        glsafe(::glDisable(GL_DEPTH_TEST));

        if (is_transparent) glsafe(::glDisable(GL_BLEND));

        shader->stop_using();
#if !ENABLE_LEGACY_OPENGL_REMOVAL
        glsafe(::glPopMatrix());
#endif // !ENABLE_LEGACY_OPENGL_REMOVAL
    }

    bool is_surface_dragging = m_temp_transformation.has_value();
    // Do NOT render rotation grabbers when dragging object
    bool is_rotate_by_grabbers = m_dragging;
    if (!is_surface_dragging && 
        (!m_parent.is_dragging() || is_rotate_by_grabbers)) {
        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
        m_rotate_gizmo.render();
    }
}

#if ENABLE_RAYCAST_PICKING
void GLGizmoEmboss::on_register_raycasters_for_picking(){
    m_rotate_gizmo.register_raycasters_for_picking();
}
void GLGizmoEmboss::on_unregister_raycasters_for_picking(){
    m_rotate_gizmo.unregister_raycasters_for_picking();
}
#else // !ENABLE_RAYCAST_PICKING
void GLGizmoEmboss::on_render_for_picking() {
    m_rotate_gizmo.render_for_picking();
}
#endif // ENABLE_RAYCAST_PICKING

#ifdef SHOW_FINE_POSITION
// draw suggested position of window
static void draw_fine_position(const Selection &selection,
                               const Size      &canvas,
                               const ImVec2    &windows_size)
{
    const Selection::IndicesList& indices = selection.get_volume_idxs();
    // no selected volume
    if (indices.empty()) return;
    const GLVolume *volume = selection.get_volume(*indices.begin());
    // bad volume selected (e.g. deleted one)
    if (volume == nullptr) return;

    const Camera   &camera = wxGetApp().plater()->get_camera();
    Slic3r::Polygon hull   = CameraUtils::create_hull2d(camera, *volume);
    ImVec2          canvas_size(canvas.get_width(), canvas.get_height());
    ImVec2 offset = ImGuiWrapper::suggest_location(windows_size, hull,
                                                   canvas_size);
    Slic3r::Polygon rect(
        {Point(offset.x, offset.y), Point(offset.x + windows_size.x, offset.y),
         Point(offset.x + windows_size.x, offset.y + windows_size.y),
         Point(offset.x, offset.y + windows_size.y)});
    ImGuiWrapper::draw(hull);
    ImGuiWrapper::draw(rect);
}
#endif // SHOW_FINE_POSITION

#ifdef DRAW_PLACE_TO_ADD_TEXT
static void draw_place_to_add_text()
{
    ImVec2             mp = ImGui::GetMousePos();
    Vec2d              mouse_pos(mp.x, mp.y);
    const Camera      &camera = wxGetApp().plater()->get_camera();
    Vec3d              p1 = CameraUtils::get_z0_position(camera, mouse_pos);
    std::vector<Vec3d> rect3d{p1 + Vec3d(5, 5, 0), p1 + Vec3d(-5, 5, 0),
                              p1 + Vec3d(-5, -5, 0), p1 + Vec3d(5, -5, 0)};
    Points             rect2d = CameraUtils::project(camera, rect3d);
    ImGuiWrapper::draw(Slic3r::Polygon(rect2d));
}
#endif // DRAW_PLACE_TO_ADD_TEXT

#ifdef SHOW_OFFSET_DURING_DRAGGING
static void draw_mouse_offset(const std::optional<Vec2d> &offset)
{
    if (!offset.has_value()) return;
    // debug draw
    auto   draw_list = ImGui::GetOverlayDrawList();
    ImVec2 p1        = ImGui::GetMousePos();
    ImVec2 p2(p1.x + offset->x(), p1.y + offset->y());
    ImU32  color     = ImGui::GetColorU32(ImGuiWrapper::COL_ORANGE_LIGHT);
    float  thickness = 3.f;
    draw_list->AddLine(p1, p2, color, thickness);
}
#endif // SHOW_OFFSET_DURING_DRAGGING

void GLGizmoEmboss::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_gui_cfg.has_value()) initialize();
    set_volume_by_selection();

    // Do not render window for not selected text volume
    if (m_volume == nullptr || !m_volume->text_configuration.has_value()) {
        close();
        return;
    } 

    // TODO: fix width - showing scroll in first draw of advanced.
    const ImVec2 &min_window_size = get_minimal_window_size();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, min_window_size);

#ifdef SHOW_FINE_POSITION
    draw_fine_position(m_parent.get_selection(), m_parent.get_canvas_size(), min_window_size);
#endif // SHOW_FINE_POSITION
#ifdef DRAW_PLACE_TO_ADD_TEXT
    draw_place_to_add_text();
#endif // DRAW_PLACE_TO_ADD_TEXT
#ifdef SHOW_OFFSET_DURING_DRAGGING
    draw_mouse_offset(m_dragging_mouse_offset);
#endif // SHOW_OFFSET_DURING_DRAGGING

    // check if is set window offset
    if (m_set_window_offset.has_value()) {
        ImGui::SetNextWindowPos(*m_set_window_offset, ImGuiCond_Always);
        m_set_window_offset.reset();
    }

    ImGuiWindowFlags flag = ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin(on_get_name().c_str(), nullptr, flag)) {
        // Need to pop var before draw window
        ImGui::PopStyleVar(); // WindowMinSize
        draw_window();
    } else {
        ImGui::PopStyleVar(); // WindowMinSize
    }
    ImGui::End();
}

void GLGizmoEmboss::on_set_state()
{
    // enable / disable bed from picking
    // Rotation gizmo must work through bed
    m_parent.set_raycaster_gizmos_on_top(GLGizmoBase::m_state == GLGizmoBase::On);

    m_rotate_gizmo.set_state(GLGizmoBase::m_state);

    // Closing gizmo. e.g. selecting another one
    if (GLGizmoBase::m_state == GLGizmoBase::Off) {
        // refuse outgoing during text preview
        if (false) {
            GLGizmoBase::m_state = GLGizmoBase::On;
            auto notification_manager = wxGetApp().plater()->get_notification_manager();
            notification_manager->push_notification(
                NotificationType::CustomNotification,
                NotificationManager::NotificationLevel::RegularNotificationLevel,
                _u8L("ERROR: Wait until ends or Cancel process."));
            return;
        }
        m_volume = nullptr;
        // Store order and last activ index into app.ini
        // TODO: what to do when can't store into file?
        m_style_manager.store_styles_to_app_config(false);
        remove_notification_not_valid_font();
    } else if (GLGizmoBase::m_state == GLGizmoBase::On) {
        if (!m_gui_cfg.has_value()) initialize();

        // to reload fonts from system, when install new one
        wxFontEnumerator::InvalidateCache();

        // Try(when exist) set text configuration by volume 
        set_volume(priv::get_selected_volume(m_parent.get_selection()));

        // change position of just opened emboss window
        set_fine_position();

        // when open by hyperlink it needs to show up
        // or after key 'T' windows doesn't appear
        m_parent.set_as_dirty();
    }
}

void GLGizmoEmboss::on_start_dragging() { m_rotate_gizmo.start_dragging(); }
void GLGizmoEmboss::on_stop_dragging()
{
    m_rotate_gizmo.stop_dragging();

    // TODO: when start second rotatiton previous rotation rotate draggers
    // This is fast fix for second try to rotate
    // When fixing, move grabber above text (not on side)
    m_rotate_gizmo.set_angle(PI/2);

    // apply rotation
    m_parent.do_rotate(L("Text-Rotate"));

    m_rotate_start_angle.reset();

    // recalculate for surface cut
    const FontProp &font_prop = m_style_manager.get_style().prop;
    if (font_prop.use_surface) process();
}
void GLGizmoEmboss::on_dragging(const UpdateData &data) { m_rotate_gizmo.dragging(data); }

void GLGizmoEmboss::initialize()
{
    if (m_gui_cfg.has_value()) return;

    GuiCfg cfg; // initialize by default values;    
    float line_height = ImGui::GetTextLineHeight();
    float line_height_with_spacing = ImGui::GetTextLineHeightWithSpacing();
    float space = line_height_with_spacing - line_height;

    cfg.max_style_name_width = ImGui::CalcTextSize("Maximal font name, extended").x;

    cfg.icon_width = std::ceil(line_height);
    // make size pair number
    if (cfg.icon_width % 2 != 0) ++cfg.icon_width;

    cfg.delete_pos_x = cfg.max_style_name_width + space;
    int count_line_of_text = 3;
    cfg.text_size = ImVec2(-FLT_MIN, line_height_with_spacing * count_line_of_text);
    ImVec2 letter_m_size = ImGui::CalcTextSize("M");
    int count_letter_M_in_input = 12;
    cfg.input_width = letter_m_size.x * count_letter_M_in_input;
    GuiCfg::Translations &tr = cfg.translations;
    tr.type  = _u8L("Type");
    tr.style = _u8L("Style");
    float max_style_text_width = std::max(
        ImGui::CalcTextSize(tr.type.c_str()).x,
        ImGui::CalcTextSize(tr.style.c_str()).x);
    cfg.style_offset = max_style_text_width + 3 * space;

    tr.font  = _u8L("Font");
    tr.size  = _u8L("Height");
    tr.depth = _u8L("Depth");
    float max_text_width = std::max({
        ImGui::CalcTextSize(tr.font.c_str()).x,
        ImGui::CalcTextSize(tr.size.c_str()).x,
        ImGui::CalcTextSize(tr.depth.c_str()).x});
    cfg.input_offset = max_text_width 
        + 3 * space + ImGui::GetTreeNodeToLabelSpacing();

    tr.use_surface = _u8L("Use surface");
    tr.char_gap = _u8L("Char gap");
    tr.line_gap = _u8L("Line gap");
    tr.boldness = _u8L("Boldness");
    tr.italic = _u8L("Skew ratio");
    tr.surface_distance = _u8L("Z-move");
    tr.angle = _u8L("Z-rot");
    tr.collection = _u8L("Collection");
    float max_advanced_text_width = std::max({
        ImGui::CalcTextSize(tr.use_surface.c_str()).x,
        ImGui::CalcTextSize(tr.char_gap.c_str()).x,
        ImGui::CalcTextSize(tr.line_gap.c_str()).x,
        ImGui::CalcTextSize(tr.boldness.c_str()).x,
        ImGui::CalcTextSize(tr.italic.c_str()).x,
        ImGui::CalcTextSize(tr.surface_distance.c_str()).x,
        ImGui::CalcTextSize(tr.angle.c_str()).x,
        ImGui::CalcTextSize(tr.collection.c_str()).x });
    cfg.advanced_input_offset = max_advanced_text_width
        + 3 * space + ImGui::GetTreeNodeToLabelSpacing();

    // calculate window size
    const ImGuiStyle &style = ImGui::GetStyle();
    float window_title = line_height + 2*style.FramePadding.y;
    float input_height = line_height_with_spacing + 2*style.FramePadding.y;
    float tree_header  = line_height_with_spacing;
    float window_height = 
        window_title + // window title
        cfg.text_size.y +  // text field
        input_height * 6 + // type Radios + style selector + font name +
                           // height + depth + close button
        tree_header +      // advance tree
        2 * style.WindowPadding.y;
    float window_width = cfg.style_offset + cfg.input_width + 2*style.WindowPadding.x 
        + 4 * (cfg.icon_width + space);
    cfg.minimal_window_size = ImVec2(window_width, window_height);

    // 6 = charGap, LineGap, Bold, italic, surfDist, angle
    // 4 = 1px for fix each edit image of drag float 
    float advance_height = input_height * 7 + 8;
    cfg.minimal_window_size_with_advance =
        ImVec2(cfg.minimal_window_size.x,
               cfg.minimal_window_size.y + advance_height);

    cfg.minimal_window_size_with_collections = 
        ImVec2(cfg.minimal_window_size_with_advance.x,
            cfg.minimal_window_size_with_advance.y + input_height);

    int max_style_image_width = cfg.max_style_name_width /2 -
                                2 * style.FramePadding.x;
    int max_style_image_height = 1.5 * input_height;
    cfg.max_style_image_size = Vec2i(max_style_image_width, max_style_image_height);
    cfg.face_name_size.y() = line_height_with_spacing;

    m_gui_cfg.emplace(std::move(cfg));

    init_icons();
    
    // initialize text styles
    m_style_manager.init(wxGetApp().app_config, create_default_styles());
    set_default_text();

    // Set rotation gizmo upwardrotate 
    m_rotate_gizmo.set_angle(PI/2);
}

EmbossStyles GLGizmoEmboss::create_default_styles()
{
    // https://docs.wxwidgets.org/3.0/classwx_font.html
    // Predefined objects/pointers: wxNullFont, wxNORMAL_FONT, wxSMALL_FONT, wxITALIC_FONT, wxSWISS_FONT
    return {
        WxFontUtils::create_emboss_style(*wxNORMAL_FONT, _u8L("NORMAL")), // wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT)
        WxFontUtils::create_emboss_style(*wxSMALL_FONT, _u8L("SMALL")),  // A font using the wxFONTFAMILY_SWISS family and 2 points smaller than wxNORMAL_FONT.
        WxFontUtils::create_emboss_style(*wxITALIC_FONT, _u8L("ITALIC")), // A font using the wxFONTFAMILY_ROMAN family and wxFONTSTYLE_ITALIC style and of the same size of wxNORMAL_FONT.
        WxFontUtils::create_emboss_style(*wxSWISS_FONT, _u8L("SWISS")),  // A font identic to wxNORMAL_FONT except for the family used which is wxFONTFAMILY_SWISS.
        WxFontUtils::create_emboss_style(wxFont(10, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD), _u8L("MODERN"))
    };
}

// Could exist systems without installed font so last chance is used own file
//EmbossStyle GLGizmoEmboss::create_default_style() {
//    std::string font_path = Slic3r::resources_dir() + "/fonts/NotoSans-Regular.ttf";
//    return EmbossStyle{"Default font", font_path, EmbossStyle::Type::file_path};
//}

void GLGizmoEmboss::set_default_text(){ m_text = _u8L("Embossed text"); }

#include "imgui/imgui_internal.h" // to unfocus input --> ClearActiveID
void GLGizmoEmboss::set_volume_by_selection()
{
    ModelVolume *vol = priv::get_selected_volume(m_parent.get_selection());
    // is same volume selected?
    if (vol != nullptr && m_volume == vol) return;

    // for changed volume notification is NOT valid
    remove_notification_not_valid_font();

    // Do not use focused input value when switch volume(it must swith value)
    if (m_volume != nullptr) ImGui::ClearActiveID();

    // is select embossed volume?
    if (!set_volume(vol)) {
        // Can't load so behave like adding new text
        m_volume = nullptr;
        set_default_text();    
    }
}

bool GLGizmoEmboss::set_volume(ModelVolume *volume)
{
    if (volume == nullptr) return false;
    const std::optional<TextConfiguration> tc_opt = volume->text_configuration;
    if (!tc_opt.has_value()) return false;
    const TextConfiguration &tc    = *tc_opt;
    const EmbossStyle       &style = tc.style;

    auto has_same_name = [&style](const StyleManager::Item &style_item) -> bool {
        const EmbossStyle &es = style_item.style;
        return es.name == style.name;
    };

    wxFont wx_font;
    bool is_path_changed = false;
    if (style.type == WxFontUtils::get_actual_type())
        wx_font = WxFontUtils::load_wxFont(style.path);
    if (!wx_font.IsOk()) {
        create_notification_not_valid_font(tc);
        // Try create similar wx font
        wx_font = WxFontUtils::create_wxFont(style);
        is_path_changed = wx_font.IsOk();
    }

    const auto& styles = m_style_manager.get_styles();
    auto it = std::find_if(styles.begin(), styles.end(), has_same_name);
    if (it == styles.end()) {
        // style was not found
        if (wx_font.IsOk())
            m_style_manager.load_style(style, wx_font);
    } else {
        size_t style_index = it - styles.begin();
        if (!m_style_manager.load_style(style_index)) {
            // can`t load stored style
            m_style_manager.erase(style_index);
            if (wx_font.IsOk())
                m_style_manager.load_style(style, wx_font);

        } else {
            // stored style is loaded, now set modification of style
            m_style_manager.get_style() = style;
            m_style_manager.set_wx_font(wx_font);
        }
    }

    if (is_path_changed) {
        std::string path = WxFontUtils::store_wxFont(wx_font);
        m_style_manager.get_style().path = path;
    }

    m_text   = tc.text;
    m_volume = volume;

    // store volume state before edit
    m_unmodified_volume = {*volume->get_mesh_shared_ptr(), // copy
                           tc, volume->get_matrix(), volume->name};

    return true;
}

ModelVolume *priv::get_model_volume(const GLVolume *gl_volume, const ModelObject *object)
{
    int volume_id = gl_volume->volume_idx();
    if (volume_id < 0 || static_cast<size_t>(volume_id) >= object->volumes.size()) return nullptr;
    return object->volumes[volume_id];
}

ModelVolume *priv::get_model_volume(const GLVolume *gl_volume, const ModelObjectPtrs &objects)
{
    int object_id = gl_volume->object_idx();
    if (object_id < 0 || static_cast<size_t>(object_id) >= objects.size()) return nullptr;
    return get_model_volume(gl_volume, objects[object_id]);
}

ModelVolume *priv::get_selected_volume(const Selection &selection)
{
    int object_idx = selection.get_object_idx();
    // is more object selected?
    if (object_idx == -1) return nullptr;

    auto volume_idxs = selection.get_volume_idxs();
    // is more volumes selected?
    if (volume_idxs.size() != 1) return nullptr;
    unsigned int    vol_id_gl = *volume_idxs.begin();
    const GLVolume *vol_gl    = selection.get_volume(vol_id_gl);
    const ModelObjectPtrs &objects = selection.get_model()->objects;
    return get_model_volume(vol_gl, objects);
}

// Run Job on main thread (blocking) - ONLY DEBUG
static inline void execute_job(std::shared_ptr<Job> j)
{
    struct MyCtl : public Job::Ctl
    {
        void update_status(int st, const std::string &msg = "") override{};
        bool was_canceled() const override { return false; }
        std::future<void> call_on_main_thread(std::function<void()> fn) override
        {
            return std::future<void>{};
        }
    } ctl;
    j->process(ctl);
    wxGetApp().plater()->CallAfter([j]() {
        std::exception_ptr e_ptr = nullptr;
        j->finalize(false, e_ptr);
    });
}

bool GLGizmoEmboss::process()
{
    // no volume is selected -> selection from right panel
    assert(m_volume != nullptr);
    if (m_volume == nullptr) return false;

    // without text there is nothing to emboss
    if (m_text.empty()) return false;

    // exist loaded font file?
    if (!m_style_manager.is_activ_font()) return false;
    
    // Cancel previous Job, when it is in process
    // Can't use cancel, because I want cancel only previous EmbossUpdateJob no other jobs
    // worker.cancel();
    // Cancel only EmbossUpdateJob no others
    if (m_update_job_cancel != nullptr)
        m_update_job_cancel->store(true);
    // create new shared ptr to cancel new job
    m_update_job_cancel = std::make_shared<std::atomic<bool> >(false);
    DataUpdate data{priv::create_emboss_data_base(m_text, m_style_manager), m_volume->id(), m_update_job_cancel};

    std::unique_ptr<Job> job = nullptr;

    // check cutting from source mesh
    bool &use_surface = data.text_configuration.style.prop.use_surface;
    bool  is_object   = m_volume->get_object()->volumes.size() == 1;
    if (use_surface && is_object) 
        use_surface = false;
    
    if (use_surface) {
        // Model to cut surface from.
        SurfaceVolumeData::ModelSources sources = create_volume_sources(m_volume);
        if (sources.empty()) return false;

        Transform3d text_tr = m_volume->get_matrix();
        auto& fix_3mf = m_volume->text_configuration->fix_3mf_tr;
        if (fix_3mf.has_value())
            text_tr = text_tr * fix_3mf->inverse();

        bool is_outside = m_volume->is_model_part();
        // check that there is not unexpected volume type
        assert(is_outside || m_volume->is_negative_volume() ||
               m_volume->is_modifier());
        UpdateSurfaceVolumeData surface_data{std::move(data), text_tr, is_outside, std::move(sources)};
        job = std::make_unique<UpdateSurfaceVolumeJob>(std::move(surface_data));                  
    } else {
        job = std::make_unique<UpdateJob>(std::move(data));
    }

    //*    
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::move(job));
    /*/ // Run Job on main thread (blocking) - ONLY DEBUG
    execute_job(std::move(job));
    // */

    // notification is removed befor object is changed by job
    remove_notification_not_valid_font();
    return true;
}

void GLGizmoEmboss::close()
{
    // remove volume when text is empty
    if (m_volume != nullptr && 
        m_volume->text_configuration.has_value() &&
        is_text_empty(m_text)) {
        Plater &p = *wxGetApp().plater();
        if (is_text_object(m_volume)) {
            // delete whole object
            p.remove(m_parent.get_selection().get_object_idx());
        } else {
            // delete text volume
            p.remove_selected();
        }
    }

    // prepare for new opening
    m_unmodified_volume.reset();

    // close gizmo == open it again
    auto& mng = m_parent.get_gizmos_manager();
    if (mng.get_current_type() == GLGizmosManager::Emboss)
        mng.open_gizmo(GLGizmosManager::Emboss);
}

void GLGizmoEmboss::discard_and_close() { 
    if (!m_unmodified_volume.has_value()) return;    
    m_volume->set_transformation(m_unmodified_volume->tr);
    UpdateJob::update_volume(m_volume, std::move(m_unmodified_volume->tm), m_unmodified_volume->tc, m_unmodified_volume->name);
    close();    

    //auto plater = wxGetApp().plater();
    // 2 .. on set state off, history is squashed into 'emboss_begin' and 'emboss_end'
    //plater->undo_to(2); // undo before open emboss gizmo
    // TODO: need fix after move to find correct undo timestamp or different approach
    // It is weird ford user that after discard changes it is moving with history

    // NOTE: Option to remember state before edit:
    //  * Need triangle mesh(memory consuming), volume name, transformation + TextConfiguration
    //  * Can't revert volume id.
    //  * Need to refresh a lot of stored data. More info in implementation EmbossJob.cpp -> update_volume()
    //  * Volume containing 3mf fix transformation - needs work around
}

void scopeguard_test() {
    bool v = false;
    {
        ScopeGuard sg;
        if (!v) { 
            v = true;
            sg = ScopeGuard([&v]() { 
                if(!v) wxMessageBox("Guard is called twice.");
                v = false; 
            });
            if (!v) wxMessageBox("v should be true in condition.");
        }
        if (!v) wxMessageBox("v should be true in scope.");
    }
    if (v) wxMessageBox("v should NOT be true.");
}

void GLGizmoEmboss::draw_window()
{
#ifdef ALLOW_DEBUG_MODE
    if (ImGui::Button("re-process")) process();
    if (ImGui::Button("add svg")) choose_svg_file();
#endif //  ALLOW_DEBUG_MODE

    bool is_activ_font = m_style_manager.is_activ_font();
    if (!is_activ_font)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _L("Warning: No font is selected. Select correct one."));
    
    // Disable all except selection of font, when open text from 3mf with unknown font
    m_imgui->disabled_begin(m_is_unknown_font);
    ScopeGuard unknown_font_sc([&]() { 
        m_imgui->disabled_end(); 
    });

    scopeguard_test();

    draw_text_input();
    draw_model_type();
    draw_style_list();
    m_imgui->disabled_begin(!is_activ_font);
    ImGui::TreePush();
    draw_style_edit();
    ImGui::TreePop();

    // close advanced style property when unknown font is selected
    if (m_is_unknown_font && m_is_advanced_edit_style) 
        ImGui::SetNextTreeNodeOpen(false);

    if (ImGui::TreeNode(_u8L("advanced").c_str())) {
        if (!m_is_advanced_edit_style) {
            set_minimal_window_size(true);
        } else {
            draw_advanced();
        }
        ImGui::TreePop();
    } else if (m_is_advanced_edit_style) 
        set_minimal_window_size(false);
    m_imgui->disabled_end(); // !is_activ_font
       
#ifdef SHOW_WX_FONT_DESCRIPTOR
    if (is_selected_style)
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, m_style_manager.get_style().path);
#endif // SHOW_WX_FONT_DESCRIPTOR

    ImGui::PushStyleColor(ImGuiCol_Button, ImGuiWrapper::COL_GREY_DARK);
    if (ImGui::Button(_u8L("Close").c_str()))
        discard_and_close();
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Discard changes on embossed text and close.").c_str());
    ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Button(_u8L("Apply").c_str())) {
        if (m_is_unknown_font) {
            process();
        } else {
            close();
        }
    }

#ifdef SHOW_CONTAIN_3MF_FIX
    if (m_volume!=nullptr &&
        m_volume->text_configuration.has_value() &&
        m_volume->text_configuration->fix_3mf_tr.has_value()) {
        ImGui::SameLine();
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, ".3mf");
        if (ImGui::IsItemHovered()) {
            Transform3d &fix = *m_volume->text_configuration->fix_3mf_tr;
            std::stringstream ss;
            ss << fix.matrix();            
            std::string filename = (m_volume->source.input_file.empty())? "unknown.3mf" :
                                   m_volume->source.input_file + ".3mf";
            ImGui::SetTooltip("Text configuation contain \n"
                              "Fix Transformation Matrix \n"
                              "%s\n"
                              "loaded from \"%s\" file.",
                              ss.str().c_str(), filename.c_str()
                );
        }
    }
#endif // SHOW_CONTAIN_3MF_FIX
#ifdef SHOW_ICONS_TEXTURE    
    auto &t = m_icons_texture;
    ImGui::Image((void *) t.get_id(), ImVec2(t.get_width(), t.get_height()));
#endif //SHOW_ICONS_TEXTURE
#ifdef SHOW_IMGUI_ATLAS
    const auto &atlas = m_style_manager.get_atlas();
    ImGui::Image(atlas.TexID, ImVec2(atlas.TexWidth, atlas.TexHeight));
#endif // SHOW_IMGUI_ATLAS
}

void GLGizmoEmboss::draw_text_input()
{
    auto create_range_text_prep = [&mng = m_style_manager, &text = m_text, &exist_unknown = m_text_contain_unknown_glyph]() {
        auto& ff = mng.get_font_file_with_cache();
        assert(ff.has_value());
        const auto &cn = mng.get_font_prop().collection_number;
        unsigned int font_index = (cn.has_value()) ? *cn : 0;
        return create_range_text(text, *ff.font_file, font_index, &exist_unknown);
    };
    
    ImFont *imgui_font = m_style_manager.get_imgui_font();
    if (imgui_font == nullptr) {
        // try create new imgui font
        m_style_manager.create_imgui_font(create_range_text_prep());
        imgui_font = m_style_manager.get_imgui_font();
    }
    bool exist_font = 
        imgui_font != nullptr &&
        imgui_font->IsLoaded() &&
        imgui_font->Scale > 0.f &&
        imgui_font->ContainerAtlas != nullptr;
    // NOTE: Symbol fonts doesn't have atlas 
    // when their glyph range is out of language character range
    if (exist_font) ImGui::PushFont(imgui_font);

    // show warning about incorrectness view of font
    std::string warning;
    std::string tool_tip;
    if (!exist_font) {
        warning  = _u8L("Can't write text by selected font.");
        tool_tip = _u8L("Try to choose another font.");
    } else {
        std::string who;
        auto        append_warning = [&who, &tool_tip](std::string w, std::string t) {
            if (!w.empty()) {
                if (!who.empty()) who += " & ";
                who += w;
            }
            if (!t.empty()) {
                if (!tool_tip.empty()) tool_tip += "\n";
                tool_tip += t;
            }
        };
        if (is_text_empty(m_text)) append_warning(_u8L("Empty"), _u8L("Embossed text can NOT contain only white spaces."));
        if (m_text_contain_unknown_glyph)
            append_warning(_u8L("Bad symbol"), _u8L("Text contain character glyph (represented by '?') unknown by font."));

        const FontProp &prop = m_style_manager.get_font_prop();
        if (prop.skew.has_value()) append_warning(_u8L("Skew"), _u8L("Unsupported visualization of font skew for text input."));
        if (prop.boldness.has_value()) append_warning(_u8L("Boldness"), _u8L("Unsupported visualization of font boldness for text input."));
        if (prop.line_gap.has_value())
            append_warning(_u8L("Line gap"), _u8L("Unsupported visualization of gap between lines inside text input."));
        auto &ff         = m_style_manager.get_font_file_with_cache();
        float imgui_size = StyleManager::get_imgui_font_size(prop, *ff.font_file);
        if (imgui_size > StyleManager::max_imgui_font_size)
            append_warning(_u8L("To tall"), _u8L("Diminished font height inside text input."));
        if (imgui_size < StyleManager::min_imgui_font_size)
            append_warning(_u8L("To small"), _u8L("Enlarged font height inside text input."));
        if (!who.empty()) warning = GUI::format(_L("%1% is NOT shown."), who);
    }

    // add border around input when warning appears
#ifndef __APPLE__
    ScopeGuard input_border_sg;
    if (!warning.empty()) { 
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORANGE_LIGHT);
        input_border_sg = ScopeGuard([]() { ImGui::PopStyleColor(); ImGui::PopStyleVar(); });
    }
#endif

    // flag for extend font ranges if neccessary
    // ranges can't be extend during font is activ(pushed)
    std::string range_text;
    float  window_height  = ImGui::GetWindowHeight();
    float  minimal_height = get_minimal_window_size().y;
    float  extra_height   = window_height - minimal_height;
    ImVec2 text_size(m_gui_cfg->text_size.x,
                     m_gui_cfg->text_size.y + extra_height);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_AutoSelectAll;
    if (ImGui::InputTextMultiline("##Text", &m_text, text_size, flags)) {
        process();
        range_text = create_range_text_prep();
    }

    if (exist_font) ImGui::PopFont();

    if (!warning.empty()) {
        if (ImGui::IsItemHovered() && !tool_tip.empty())
            ImGui::SetTooltip("%s", tool_tip.c_str());
        ImVec2 cursor = ImGui::GetCursorPos();
        float width = ImGui::GetContentRegionAvailWidth();
        ImVec2 size = ImGui::CalcTextSize(warning.c_str());
        ImVec2 padding = ImGui::GetStyle().FramePadding;
        ImGui::SetCursorPos(ImVec2(width - size.x + padding.x,
                                   cursor.y - size.y - padding.y));
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, warning);
        ImGui::SetCursorPos(cursor);
    }

    // NOTE: must be after ImGui::font_pop() 
    //          -> imgui_font has to be unused
    // IMPROVE: only extend not clear
    // Extend font ranges
    if (!range_text.empty() &&
        !m_imgui->contain_all_glyphs(imgui_font, range_text) ) { 
        m_style_manager.clear_imgui_font(); 
        m_style_manager.create_imgui_font(range_text);
    }
}

#include <boost/functional/hash.hpp>
#include "wx/hashmap.h"
std::size_t hash_value(wxString const &s)
{
    boost::hash<std::string> hasher;
    return hasher(s.ToStdString());
}

static std::string concat(std::vector<wxString> data) {
    std::stringstream ss;
    for (const auto &d : data) 
        ss << d.c_str() << ", ";
    return ss.str();
}

#include <boost/filesystem.hpp>
static boost::filesystem::path get_fontlist_cache_path()
{
    return boost::filesystem::path(data_dir()) / "cache" / "fonts.cereal";
}

// cache font list by cereal
#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cereal/archives/binary.hpp>

// increase number when change struct FacenamesSerializer
#define FACENAMES_VERSION 1
struct FacenamesSerializer
{
    // hash number for unsorted vector of installed font into system
    size_t hash = 0;
    // assumption that is loadable
    std::vector<wxString> good;
    // Can't load for some reason
    std::vector<wxString> bad;
};

template<class Archive> void save(Archive &archive, wxString const &d)
{ std::string s(d.ToUTF8().data()); archive(s);}
template<class Archive> void load(Archive &archive, wxString &d)
{ std::string s; archive(s); d = s;} 

template<class Archive> void serialize(Archive &ar, FacenamesSerializer &t, const std::uint32_t version)
{
    // When performing a load, the version associated with the class
    // is whatever it was when that data was originally serialized
    // When we save, we'll use the version that is defined in the macro
    if (version != FACENAMES_VERSION) return;
    ar(t.hash, t.good, t.bad);
}
CEREAL_CLASS_VERSION(FacenamesSerializer, FACENAMES_VERSION); // register class version

#include <boost/nowide/fstream.hpp>
bool GLGizmoEmboss::store(const Facenames &facenames) {
    std::string cache_path = get_fontlist_cache_path().string();
    boost::nowide::ofstream file(cache_path, std::ios::binary);
    cereal::BinaryOutputArchive archive(file);
    std::vector<wxString> good;
    good.reserve(facenames.faces.size());
    for (const FaceName &face : facenames.faces) good.push_back(face.wx_name);
    FacenamesSerializer data = {facenames.hash, good, facenames.bad};

    assert(std::is_sorted(data.bad.begin(), data.bad.end()));
    assert(std::is_sorted(data.good.begin(), data.good.end()));

    try {
        archive(data);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to write fontlist cache - " << cache_path << ex.what();
        return false;
    }
    return true;
}

bool GLGizmoEmboss::load(Facenames &facenames) {
    boost::filesystem::path path = get_fontlist_cache_path();
    std::string             path_str = path.string();
    if (!boost::filesystem::exists(path)) {
        BOOST_LOG_TRIVIAL(warning) << "Fontlist cache - '" << path_str << "' does not exists.";
        return false;
    }
    boost::nowide::ifstream file(path_str, std::ios::binary);
    cereal::BinaryInputArchive archive(file);
    
    FacenamesSerializer data;
    try {
        archive(data);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load fontlist cache - '" << path_str << "'. Exception: " << ex.what();
        return false;
    }

    assert(std::is_sorted(data.bad.begin(), data.bad.end()));
    assert(std::is_sorted(data.good.begin(), data.good.end()));

    facenames.hash = data.hash;
    facenames.faces.reserve(data.good.size());
    for (const wxString &face : data.good)
        facenames.faces.push_back({face});
    facenames.bad = data.bad;
    return true;
}

void GLGizmoEmboss::init_face_names() {
    Timer t("enumerate_fonts");
    if (m_face_names.is_init) return;
    m_face_names.is_init = true;

    // to reload fonts from system, when install new one
    wxFontEnumerator::InvalidateCache();

    auto create_truncated_names = [&facenames = m_face_names, &width = m_gui_cfg->face_name_max_width]() {
        for (FaceName &face : facenames.faces) {
            std::string name_str(face.wx_name.ToUTF8().data());
            face.name_truncated = ImGuiWrapper::trunc(name_str, width);
        }
    };

    // try load cache
    // Only not OS enumerated face has hash value 0
    if (m_face_names.hash == 0) {
        load(m_face_names);
        create_truncated_names();
    }

    using namespace std::chrono;
    steady_clock::time_point enumerate_start = steady_clock::now();
    ScopeGuard sg([&enumerate_start, &face_names = m_face_names]() {
        steady_clock::time_point enumerate_end = steady_clock::now();
        long long enumerate_duration = duration_cast<milliseconds>(enumerate_end - enumerate_start).count();
        BOOST_LOG_TRIVIAL(info) << "OS enumerate " << face_names.faces.size() << " fonts "
                                << "(+ " << face_names.bad.size() << " can't load "
                                << "= " << face_names.faces.size() + face_names.bad.size() << " fonts) "
                                << "in " << enumerate_duration << " ms\n" << concat(face_names.bad);
    });
    wxArrayString facenames = wxFontEnumerator::GetFacenames(m_face_names.encoding);
    size_t hash = boost::hash_range(facenames.begin(), facenames.end());
    // Zero value is used as uninitialized hash
    if (hash == 0) hash = 1;
    // check if it is same as last time
    if (m_face_names.hash == hash) { 
        // no new installed font
        BOOST_LOG_TRIVIAL(info) << "Same FontNames hash, cache is used. " 
            << "For clear cache delete file: " << get_fontlist_cache_path().string();
        return;
    }

    BOOST_LOG_TRIVIAL(info) << ((m_face_names.hash == 0) ?
        "FontName list is generate from scratch." :
        "Hash are different. Only previous bad fonts are used and set again as bad");
    m_face_names.hash = hash;
    
    // validation lambda
    auto is_valid_font = [encoding = m_face_names.encoding, bad = m_face_names.bad /*copy*/](const wxString &name) {
        if (name.empty()) return false;

        // vertical font start with @, we will filter it out
        // Not sure if it is only in Windows so filtering is on all platforms
        if (name[0] == '@') return false;        

        // previously detected bad font
        auto it = std::lower_bound(bad.begin(), bad.end(), name);
        if (it != bad.end() && *it == name) return false;

        wxFont wx_font(wxFontInfo().FaceName(name).Encoding(encoding));
        //*
        // Faster chech if wx_font is loadable but not 100%
        // names could contain not loadable font
        if (!WxFontUtils::can_load(wx_font)) return false;

        /*/
        // Slow copy of font files to try load font
        // After this all files are loadable
        auto font_file = WxFontUtils::create_font_file(wx_font);
        if (font_file == nullptr) 
            return false; // can't create font file
        // */
        return true;
    };

    m_face_names.faces.clear();
    m_face_names.bad.clear();
    m_face_names.faces.reserve(facenames.size());
    std::sort(facenames.begin(), facenames.end());
    for (const wxString &name : facenames) {
        if (is_valid_font(name)) {
            m_face_names.faces.push_back({name});
        }else{
            m_face_names.bad.push_back(name);
        }
    }
    assert(std::is_sorted(m_face_names.bad.begin(), m_face_names.bad.end()));
    create_truncated_names();
    store(m_face_names);
}

// create texture for visualization font face
void GLGizmoEmboss::init_font_name_texture() {
    Timer t("init_font_name_texture");
    // check if already exists
    GLuint &id = m_face_names.texture_id; 
    if (id != 0) return;
    // create texture for font
    GLenum target = GL_TEXTURE_2D;
    glsafe(::glGenTextures(1, &id));
    glsafe(::glBindTexture(target, id));
    glsafe(::glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    const Vec2i &size = m_gui_cfg->face_name_size;
    GLint w = size.x(), h = m_face_names.count_cached_textures * size.y();
    std::vector<unsigned char> data(4*w * h, {0});
    const GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
    const GLint level = 0, internal_format = GL_RGBA, border = 0;
    glsafe(::glTexImage2D(target, level, internal_format, w, h, border, format, type, data.data()));

    // bind default texture
    GLuint no_texture_id = 0;
    glsafe(::glBindTexture(target, no_texture_id));

    // clear info about creation of texture - no one is initialized yet
    for (FaceName &face : m_face_names.faces) { 
        face.cancel = nullptr;
        face.is_created = nullptr;
    }
}

void GLGizmoEmboss::draw_font_preview(FaceName& face, bool is_visible)
{
    unsigned int &count_opened_fonts = m_face_names.count_opened_font_files; 
    ImVec2 size(m_gui_cfg->face_name_size.x(), m_gui_cfg->face_name_size.y());
    // set to pixel 0,0 in texture
    ImVec2      uv0(0.f, 0.f), uv1(1.f / size.x, 1.f / size.y / m_face_names.count_cached_textures);
    ImTextureID tex_id = (void *) (intptr_t) m_face_names.texture_id;
    if (face.is_created != nullptr) {
        if (*face.is_created) {
            size_t texture_index = face.texture_index;
            uv0                  = ImVec2(0.f, texture_index / (float) m_face_names.count_cached_textures),
            uv1                  = ImVec2(1.f, (texture_index + 1) / (float) m_face_names.count_cached_textures);
        } else if (!is_visible) {
            face.is_created = nullptr;
            face.cancel->store(true);
        }
    } else if (is_visible && count_opened_fonts < m_gui_cfg->max_count_opened_font_files) {
        ++count_opened_fonts;
        face.cancel     = std::make_shared<std::atomic_bool>(false);
        face.is_created = std::make_shared<bool>(false);

        std::string text = m_text.empty() ? "AaBbCc" : m_text;

        const unsigned char gray_level = 5;
        // format type and level must match to texture data
        const GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
        const GLint  level = 0;
        // select next texture index
        size_t texture_index = (m_face_names.texture_index + 1) % m_face_names.count_cached_textures;
        // set previous cach as deleted
        for (FaceName &f : m_face_names.faces)
            if (f.texture_index == texture_index) {
                if (f.cancel != nullptr) f.cancel->store(true);
                f.is_created = nullptr;
            }

        m_face_names.texture_index = texture_index;
        face.texture_index         = texture_index;

        // clear texture


        // render text to texture
        FontImageData data{
            text,
            face.wx_name,
            m_face_names.encoding,
            m_face_names.texture_id,
            m_face_names.texture_index,
            m_gui_cfg->face_name_size,
            gray_level,
            format,
            type,
            level,
            &count_opened_fonts,
            face.cancel,    // copy
            face.is_created // copy
        };
        auto  job    = std::make_unique<CreateFontImageJob>(std::move(data));
        auto &worker = wxGetApp().plater()->get_ui_job_worker();
        queue_job(worker, std::move(job));
    }

    ImGui::SameLine(m_gui_cfg->face_name_texture_offset_x);
    ImGui::Image(tex_id, size, uv0, uv1);
}

bool GLGizmoEmboss::select_facename(const wxString &facename)
{
    if (!wxFontEnumerator::IsValidFacename(facename)) return false;
    // Select font
    const wxFontEncoding &encoding = m_face_names.encoding;
    wxFont                wx_font(wxFontInfo().FaceName(facename).Encoding(encoding));
    if (!wx_font.IsOk()) return false;
    // wx font could change source file by size of font
    int point_size = static_cast<int>(m_style_manager.get_font_prop().size_in_mm);
    wx_font.SetPointSize(point_size);
    if (!m_style_manager.set_wx_font(wx_font)) return false;
    process();
    return true;
}

void GLGizmoEmboss::draw_font_list()
{
    // Set partial
    wxString actual_face_name;
    if (m_style_manager.is_activ_font()) {
        const std::optional<wxFont> &wx_font_opt = m_style_manager.get_wx_font();
        if (wx_font_opt.has_value())
            actual_face_name = wx_font_opt->GetFaceName();
    }
    // name of actual selected font
    const char * selected = (!actual_face_name.empty()) ?
        actual_face_name.ToUTF8().data() : " --- ";

    // Do not remove font face during enumeration
    // When deletation of font appear this variable is set
    std::optional<size_t> del_index;

    // When is unknown font is inside .3mf only font selection is allowed
    // Stop Imgui disable + Guard again start disabling
    ScopeGuard unknown_font_sc;
    if (m_is_unknown_font) {
        m_imgui->disabled_end(); 
        unknown_font_sc = ScopeGuard([&]() { 
            m_imgui->disabled_begin(true); 
        });
    }

    if (ImGui::BeginCombo("##font_selector", selected)) {
        if (!m_face_names.is_init) init_face_names();
        if (m_face_names.texture_id == 0) init_font_name_texture();
        for (FaceName &face : m_face_names.faces) {
            const wxString &wx_face_name = face.wx_name;
            size_t index = &face - &m_face_names.faces.front();
            ImGui::PushID(index);
            ScopeGuard sg([]() { ImGui::PopID(); });
            bool is_selected = (actual_face_name == wx_face_name);
            ImVec2 selectable_size(0, m_gui_cfg->face_name_size.y());
            ImGuiSelectableFlags flags = 0;
            if (ImGui::Selectable(face.name_truncated.c_str(), is_selected, flags, selectable_size)) {
                if (!select_facename(wx_face_name)) {
                    del_index = index;
                    wxMessageBox(GUI::format(_L("Font face \"%1%\" can't be selected."), wx_face_name));
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", wx_face_name.ToUTF8().data());
            if (is_selected) ImGui::SetItemDefaultFocus();
            draw_font_preview(face, ImGui::IsItemVisible());
        }        
#ifdef SHOW_FONT_COUNT
        ImGui::TextColored(ImGuiWrapper::COL_GREY_LIGHT, "Count %d",
                           static_cast<int>(m_face_names.names.size()));
#endif // SHOW_FONT_COUNT
        ImGui::EndCombo();
    } else if (m_face_names.is_init) {
        // Just one after close combo box        
        // free texture and set id to zero

        m_face_names.is_init = false;
        // cancel all process for generation of texture
        for (FaceName &face : m_face_names.faces)
            if (face.cancel != nullptr) face.cancel->store(true);
        glsafe(::glDeleteTextures(1, &m_face_names.texture_id));
        m_face_names.texture_id = 0;
    }

    // delete unloadable face name when try to use
    if (del_index.has_value()) {
        auto face = m_face_names.faces.begin() + (*del_index);
        std::vector<wxString>& bad = m_face_names.bad;
        // sorted insert into bad fonts
        auto it = std::upper_bound(bad.begin(), bad.end(), face->wx_name);
        bad.insert(it, face->wx_name);
        m_face_names.faces.erase(face);
        // update cached file
        store(m_face_names);
    }

#ifdef ALLOW_ADD_FONT_BY_FILE
    ImGui::SameLine();
    // select font file by file browser
    if (draw_button(IconType::open_file)) {
        if (choose_true_type_file()) { 
            process();
        }
    } else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("add file with font(.ttf, .ttc)").c_str());
#endif //  ALLOW_ADD_FONT_BY_FILE

#ifdef ALLOW_ADD_FONT_BY_OS_SELECTOR
    ImGui::SameLine();
    if (draw_button(IconType::system_selector)) {
        if (choose_font_by_wxdialog()) {
            process();
        }
    } else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Open dialog for choose from fonts.").c_str());
#endif //  ALLOW_ADD_FONT_BY_OS_SELECTOR

}

void GLGizmoEmboss::draw_model_type()
{
    bool is_last_solid_part = is_text_object(m_volume);
    const char * label = m_gui_cfg->translations.type.c_str();
    if (is_last_solid_part) {
        ImVec4 color{.5f, .5f, .5f, 1.f};
        m_imgui->text_colored(color, label);
    } else {
        ImGui::Text("%s", label);
    }
    ImGui::SameLine(m_gui_cfg->style_offset);

    std::optional<ModelVolumeType> new_type;
    ModelVolumeType modifier = ModelVolumeType::PARAMETER_MODIFIER;
    ModelVolumeType negative = ModelVolumeType::NEGATIVE_VOLUME;
    ModelVolumeType part = ModelVolumeType::MODEL_PART;
    ModelVolumeType type = m_volume->type();

    if (type == part) { 
        draw_icon(IconType::part, IconState::hovered);
    } else {
        if (draw_button(IconType::part)) new_type = part;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Click to change text into object part.").c_str());
    }

    ImGui::SameLine();
    if (type == negative) { 
        draw_icon(IconType::negative, IconState::hovered);    
    } else {
        if (draw_button(IconType::negative, is_last_solid_part))
            new_type = negative;        
        if(ImGui::IsItemHovered()){
            if(is_last_solid_part)
                ImGui::SetTooltip("%s", _u8L("You can't change a type of the last solid part of the object.").c_str());
            else if (type != negative)
                ImGui::SetTooltip("%s", _u8L("Click to change part type into negative volume.").c_str());
        }
    }

    ImGui::SameLine();
    if (type == modifier) {
        draw_icon(IconType::modifier, IconState::hovered);  
    } else {
        if(draw_button(IconType::modifier, is_last_solid_part))
            new_type = modifier;    
        if (ImGui::IsItemHovered()) {
            if(is_last_solid_part)
                ImGui::SetTooltip("%s", _u8L("You can't change a type of the last solid part of the object.").c_str());
            else if (type != modifier)
                ImGui::SetTooltip("%s", _u8L("Click to change part type into modifier.").c_str());
        }
    }

    if (m_volume != nullptr && new_type.has_value() && !is_last_solid_part) {
        GUI_App &app    = wxGetApp();
        Plater * plater = app.plater();
        Plater::TakeSnapshot snapshot(plater, _L("Change Text Type"), UndoRedo::SnapshotType::GizmoAction);
        m_volume->set_type(*new_type);

         // Update volume position when switch from part or into part
        if (m_volume->text_configuration->style.prop.use_surface) {
            // move inside
            bool is_volume_move_inside  = (type == part);
            bool is_volume_move_outside = (*new_type == part);
            if (is_volume_move_inside || is_volume_move_outside) process();
        }

        // inspiration in ObjectList::change_part_type()
        // how to view correct side panel with objects
        ObjectList *obj_list = app.obj_list();
        wxDataViewItemArray sel = obj_list->reorder_volumes_and_get_selection(
            obj_list->get_selected_obj_idx(),
            [volume = m_volume](const ModelVolume *vol) { return vol == volume; });
        if (!sel.IsEmpty()) obj_list->select_item(sel.front());       

        // NOTE: on linux, function reorder_volumes_and_get_selection call GLCanvas3D::reload_scene(refresh_immediately = false)
        // which discard m_volume pointer and set it to nullptr also selection is cleared so gizmo is automaticaly closed
        auto &mng = m_parent.get_gizmos_manager();
        if (mng.get_current_type() != GLGizmosManager::Emboss)
            mng.open_gizmo(GLGizmosManager::Emboss);
        // TODO: select volume back - Ask @Sasa
    }
}

void GLGizmoEmboss::draw_style_rename_popup() {
    std::string& new_name = m_style_manager.get_style().name;
    const std::string &old_name = m_style_manager.get_stored_style()->name;
    std::string text_in_popup = GUI::format(_L("Rename style(%1%) for embossing text: "), old_name);
    ImGui::Text("%s", text_in_popup.c_str());
    
    bool is_unique = true;
    for (const auto &item : m_style_manager.get_styles()) {
        const EmbossStyle &style = item.style;
        if (&style == &m_style_manager.get_style())
            continue; // could be same as original name
        if (style.name == new_name) is_unique = false;
    }
    bool allow_change = false;
    if (new_name.empty()) {
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_DARK, _u8L("Name can't be empty."));
    }else if (!is_unique) { 
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_DARK, _u8L("Name has to be unique."));
    } else {
        ImGui::NewLine();
        allow_change = true;
    }

    bool store = false;
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (ImGui::InputText("##rename style", &new_name, flags) && allow_change) store = true;
    if (m_imgui->button(_L("ok"), ImVec2(0.f, 0.f), allow_change)) store = true;
    ImGui::SameLine();
    if (ImGui::Button(_u8L("cancel").c_str())) {
        new_name = old_name;
        ImGui::CloseCurrentPopup();
    }

    if (store) {
        // rename style in all objects and volumes
        for (ModelObject *mo :wxGetApp().plater()->model().objects) {
            for (ModelVolume *mv : mo->volumes) { 
                if (!mv->text_configuration.has_value()) continue;
                std::string& name = mv->text_configuration->style.name;
                if (name != old_name) continue;
                name = new_name;
            }
        }
        
        m_style_manager.rename(new_name);
        m_style_manager.store_styles_to_app_config();
        ImGui::CloseCurrentPopup();
    }
}

void GLGizmoEmboss::draw_style_rename_button()
{
    bool can_rename = m_style_manager.exist_stored_style();
    std::string title = _u8L("Rename style");
    const char * popup_id = title.c_str();
    if (draw_button(IconType::rename, !can_rename)) {
        assert(m_style_manager.get_stored_style());
        ImGui::OpenPopup(popup_id);
    }
    else if (ImGui::IsItemHovered()) {
        if (can_rename) ImGui::SetTooltip("%s", _u8L("Rename actual style.").c_str());
        else            ImGui::SetTooltip("%s", _u8L("Can't rename temporary style.").c_str());
    }
    if (ImGui::BeginPopupModal(popup_id, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        m_imgui->disable_background_fadeout_animation();
        draw_style_rename_popup();
        ImGui::EndPopup();
    }
}

void GLGizmoEmboss::draw_style_save_button(bool is_modified)
{
    if (draw_button(IconType::save, !is_modified)) {
        // save styles to app config
        m_style_manager.store_styles_to_app_config();
    }else if (ImGui::IsItemHovered()) {
        std::string tooltip;
        if (!m_style_manager.exist_stored_style()) {
            tooltip = _u8L("First Add style to list.");
        } else if (is_modified) {
            tooltip = GUI::format(_L("Save %1% style"), m_style_manager.get_style().name);
        } else {
            tooltip = _u8L("No changes to save.");
        }
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
}

void GLGizmoEmboss::draw_style_save_as_popup() {
    ImGui::Text("%s", _u8L("New name of style: ").c_str());

    // use name inside of volume configuration as temporary new name
    std::string &new_name = m_volume->text_configuration->style.name;

    bool is_unique = true;
    for (const auto &item : m_style_manager.get_styles())
        if (item.style.name == new_name) is_unique = false;
        
    bool allow_change = false;
    if (new_name.empty()) {
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_DARK, _u8L("Name can't be empty."));
    }else if (!is_unique) { 
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_DARK, _u8L("Name has to be unique."));
    } else {
        ImGui::NewLine();
        allow_change = true;
    }

    bool save_style = false;
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (ImGui::InputText("##save as style", &new_name, flags))
        save_style = true;
        
    if (m_imgui->button(_L("ok"), ImVec2(0.f, 0.f), allow_change))
        save_style = true;

    ImGui::SameLine();
    if (ImGui::Button(_u8L("cancel").c_str())){
        // write original name to volume TextConfiguration
        new_name = m_style_manager.get_style().name;
        ImGui::CloseCurrentPopup();
    }

    if (save_style && allow_change) {
        m_style_manager.add_style(new_name);
        m_style_manager.store_styles_to_app_config();
        ImGui::CloseCurrentPopup();
    }
}

void GLGizmoEmboss::draw_style_add_button()
{
    bool only_add_style = !m_style_manager.exist_stored_style();
    bool can_add        = true;
    if (only_add_style &&
        m_volume->text_configuration->style.type != WxFontUtils::get_actual_type())
        can_add = false;

    std::string title    = _u8L("Save as new style");
    const char *popup_id = title.c_str();
    // save as new style
    ImGui::SameLine();
    if (draw_button(IconType::add, !can_add)) {
        if (!m_style_manager.exist_stored_style()) {
            m_style_manager.store_styles_to_app_config(wxGetApp().app_config);
        } else {
            ImGui::OpenPopup(popup_id);
        }
    } else if (ImGui::IsItemHovered()) {
        if (!can_add) {
            ImGui::SetTooltip("%s", _u8L("Only valid font can be added to style.").c_str());
        }else if (only_add_style) {
            ImGui::SetTooltip("%s", _u8L("Add style to my list.").c_str());
        } else {
            ImGui::SetTooltip("%s", _u8L("Add as new named style.").c_str());
        }
    }

    if (ImGui::BeginPopupModal(popup_id, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        m_imgui->disable_background_fadeout_animation();
        draw_style_save_as_popup();
        ImGui::EndPopup();
    }
}

void GLGizmoEmboss::draw_delete_style_button() {
    bool is_stored  = m_style_manager.exist_stored_style();
    bool is_last    = m_style_manager.get_styles().size() == 1;
    bool can_delete = is_stored && !is_last;

    std::string title = _u8L("Remove style");
    const char * popup_id = title.c_str();
    static size_t next_style_index = std::numeric_limits<size_t>::max();
    if (draw_button(IconType::erase, !can_delete)) {
        while (true) {
            // NOTE: can't use previous loaded activ index -> erase could change index
            size_t activ_index = m_style_manager.get_style_index();
            next_style_index = (activ_index > 0) ? activ_index - 1 :
                                                   activ_index + 1;
            if (next_style_index >= m_style_manager.get_styles().size()) {
                // can't remove last font style
                // TODO: inform user
                break;
            }
            // IMPROVE: add function can_load?
            // clean unactivable styles
            if (!m_style_manager.load_style(next_style_index)) {
                m_style_manager.erase(next_style_index);
                continue;
            }

            // load back
            m_style_manager.load_style(activ_index);
            ImGui::OpenPopup(popup_id);
            break;
        }
    }

    if (ImGui::IsItemHovered()) {
        const std::string &style_name = m_style_manager.get_style().name;
        std::string tooltip;
        if (can_delete)        tooltip = GUI::format(_L("Delete \"%1%\" style."), style_name);
        else if (is_last)      tooltip = GUI::format(_L("Can't delete \"%1%\". It is last style."), style_name);
        else/*if(!is_stored)*/ tooltip = GUI::format(_L("Can't delete temporary style \"%1%\"."), style_name);        
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    if (ImGui::BeginPopupModal(popup_id)) {
        m_imgui->disable_background_fadeout_animation();
        const std::string &style_name  = m_style_manager.get_style().name;
        std::string text_in_popup = GUI::format(_L("Are you sure,\nthat you want permanently and unrecoverable \nremove style \"%1%\"?"), style_name);
        ImGui::Text("%s", text_in_popup.c_str());
        if (ImGui::Button(_u8L("Yes").c_str())) {
            size_t activ_index = m_style_manager.get_style_index();
            m_style_manager.load_style(next_style_index);
            m_style_manager.erase(activ_index);
            m_style_manager.store_styles_to_app_config(wxGetApp().app_config);
            ImGui::CloseCurrentPopup();
            process();
        }
        ImGui::SameLine();
        if (ImGui::Button(_u8L("No").c_str()))
            ImGui::CloseCurrentPopup();        
        ImGui::EndPopup();
    }
}

void GLGizmoEmboss::fix_transformation(const FontProp &from,
                                       const FontProp &to)
{
    // fix Z rotation when exists difference in styles
    const std::optional<float> &f_angle_opt = from.angle;
    const std::optional<float> &t_angle_opt = to.angle;
    if (!is_approx(f_angle_opt, t_angle_opt)) {
        // fix rotation
        float f_angle = f_angle_opt.has_value() ? *f_angle_opt : .0f;
        float t_angle = t_angle_opt.has_value() ? *t_angle_opt : .0f;
        do_rotate(t_angle - f_angle);
    }

    // fix distance (Z move) when exists difference in styles
    const std::optional<float> &f_move_opt = from.distance;
    const std::optional<float> &t_move_opt = to.distance;
    if (!is_approx(f_move_opt, t_move_opt)) {
        float f_move = f_move_opt.has_value() ? *f_move_opt : .0f;
        float t_move = t_move_opt.has_value() ? *t_move_opt : .0f;
        do_translate(Vec3d::UnitZ() * (t_move - f_move));
    }
}

void GLGizmoEmboss::draw_style_list() {
    if (!m_style_manager.is_activ_font()) return;

    const EmbossStyle *stored_style = nullptr;
    bool is_stored = m_style_manager.exist_stored_style();
    if (is_stored)
        stored_style = m_style_manager.get_stored_style();
    const EmbossStyle &actual_style = m_style_manager.get_style();
    bool is_changed = (stored_style)? !(*stored_style == actual_style) : true;    
    bool is_modified = is_stored && is_changed;

    const float &max_style_name_width = m_gui_cfg->max_style_name_width;
    std::string &trunc_name = m_style_manager.get_truncated_name();
    if (trunc_name.empty()) {
        // generate trunc name
        std::string current_name = actual_style.name;
        ImGuiWrapper::escape_double_hash(current_name);
        trunc_name = ImGuiWrapper::trunc(current_name, max_style_name_width);
    }

    if (m_style_manager.exist_stored_style())
        ImGui::Text("%s", m_gui_cfg->translations.style.c_str());
    else ImGui::TextColored(ImGuiWrapper::COL_ORANGE_LIGHT, "%s", m_gui_cfg->translations.style.c_str());

    ImGui::SameLine(m_gui_cfg->style_offset);
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);
    auto add_text_modify = [&is_modified](const std::string& name) {
        if (!is_modified) return name;
        return name + " (" + _u8L("modified") + ")";
    };
    std::optional<size_t> selected_style_index;
    if (ImGui::BeginCombo("##style_selector", add_text_modify(trunc_name).c_str())) {
        m_style_manager.init_style_images(m_gui_cfg->max_style_image_size, m_text);
        m_style_manager.init_trunc_names(max_style_name_width);
        std::optional<std::pair<size_t,size_t>> swap_indexes;
        const std::vector<StyleManager::Item> &styles = m_style_manager.get_styles();
        for (const auto &item : styles) {
            size_t index = &item - &styles.front();
            const EmbossStyle &style = item.style;
            const std::string &actual_style_name = style.name;
            ImGui::PushID(actual_style_name.c_str());
            bool is_selected = (index == m_style_manager.get_style_index());

            ImVec2 select_size(0,m_gui_cfg->max_style_image_size.y()); // 0,0 --> calculate in draw
            const std::optional<StyleManager::StyleImage> &img = item.image;            
            // allow click delete button
            ImGuiSelectableFlags_ flags = ImGuiSelectableFlags_AllowItemOverlap; 
            if (ImGui::Selectable(item.truncated_name.c_str(), is_selected, flags, select_size)) {
                selected_style_index = index;
            } else if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", actual_style_name.c_str());

            // reorder items
            if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
                if (ImGui::GetMouseDragDelta(0).y < 0.f) {
                    if (index > 0) 
                        swap_indexes = {index, index - 1};
                } else if ((index + 1) < styles.size())
                    swap_indexes = {index, index + 1};
                if (swap_indexes.has_value()) 
                    ImGui::ResetMouseDragDelta();
            }

            // draw style name
            if (img.has_value()) {
                ImGui::SameLine(max_style_name_width);
                ImGui::Image(img->texture_id, img->tex_size, img->uv0, img->uv1);
            }

            ImGui::PopID();
        }
        if (swap_indexes.has_value()) 
            m_style_manager.swap(swap_indexes->first,
                                swap_indexes->second);
        ImGui::EndCombo();
    } else {
        // do not keep in memory style images when no combo box open
        m_style_manager.free_style_images();
        if (ImGui::IsItemHovered()) {            
            std::string style_name = add_text_modify(actual_style.name);
            std::string tooltip = is_modified?
                GUI::format(_L("Modified style \"%1%\""), actual_style.name):
                GUI::format(_L("Current style is \"%1%\""), actual_style.name);
            ImGui::SetTooltip(" %s", tooltip.c_str());
        }
    }
        
    // Check whether user wants lose actual style modification
    if (selected_style_index.has_value() && is_modified) { 
        wxString title   = _L("Style modification will be lost.");
        const EmbossStyle &style = m_style_manager.get_styles()[*selected_style_index].style;        
        wxString message = GUI::format_wxstr(_L("Changing style to '%1%' will discard actual style modification.\n\n Would you like to continue anyway?"), style.name);
        MessageDialog not_loaded_style_message(nullptr, message, title, wxICON_WARNING | wxYES|wxNO);
        if (not_loaded_style_message.ShowModal() != wxID_YES) 
            selected_style_index.reset();
    }

    // selected style from combo box
    if (selected_style_index.has_value()) {
        const EmbossStyle &style = m_style_manager.get_styles()[*selected_style_index].style;
        fix_transformation(actual_style.prop, style.prop);
        if (m_style_manager.load_style(*selected_style_index)) {
            process();
        } else {
            wxString title   = _L("Not valid style.");
            wxString message = GUI::format_wxstr(_L("Style '%1%' can't be used and will be removed from list."), style.name);
            MessageDialog not_loaded_style_message(nullptr, message, title, wxOK);
            not_loaded_style_message.ShowModal();
            m_style_manager.erase(*selected_style_index);
        }
    }

    ImGui::SameLine();
    draw_style_rename_button();
        
    ImGui::SameLine();
    draw_style_save_button(is_modified);

    ImGui::SameLine();
    draw_style_add_button();

    // delete button
    ImGui::SameLine();
    draw_delete_style_button();
}

bool GLGizmoEmboss::draw_italic_button()
{
    const std::optional<wxFont> &wx_font_opt = m_style_manager.get_wx_font(); 
    const auto& ff = m_style_manager.get_font_file_with_cache();
    if (!wx_font_opt.has_value() || !ff.has_value()) { 
        draw_icon(IconType::italic, IconState::disabled);
        return false;
    }
    const wxFont& wx_font = *wx_font_opt;

    std::optional<float> &skew = m_style_manager.get_font_prop().skew;
    bool is_font_italic = skew.has_value() || WxFontUtils::is_italic(wx_font);
    if (is_font_italic) {
        // unset italic
        if (draw_clickable(IconType::italic, IconState::hovered,
                           IconType::unitalic, IconState::hovered)) {
            skew.reset();
            if (wx_font.GetStyle() != wxFontStyle::wxFONTSTYLE_NORMAL) {
                wxFont new_wx_font = wx_font; // copy
                new_wx_font.SetStyle(wxFontStyle::wxFONTSTYLE_NORMAL);
                if(!m_style_manager.set_wx_font(new_wx_font))
                    return false;
            }
            return true;
        }
        if (ImGui::IsItemHovered()) 
            ImGui::SetTooltip("%s", _u8L("Unset italic").c_str());
    } else {
        // set italic
        if (draw_button(IconType::italic)) {
            wxFont new_wx_font = wx_font; // copy
            auto new_ff = WxFontUtils::set_italic(new_wx_font, *ff.font_file);
            if (new_ff != nullptr) {
                if(!m_style_manager.set_wx_font(new_wx_font, std::move(new_ff)))
                    return false;
            } else {
                // italic font doesn't exist 
                // add skew when wxFont can't set it
                skew = 0.2f;
            }            
            return true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Set italic").c_str());
    }
    return false;
}

bool GLGizmoEmboss::draw_bold_button() {
    const std::optional<wxFont> &wx_font_opt = m_style_manager.get_wx_font();
    const auto& ff = m_style_manager.get_font_file_with_cache();
    if (!wx_font_opt.has_value() || !ff.has_value()) {
        draw_icon(IconType::bold, IconState::disabled);
        return false;
    }
    const wxFont &wx_font = *wx_font_opt;
    
    std::optional<float> &boldness = m_style_manager.get_font_prop().boldness;
    bool is_font_bold = boldness.has_value() || WxFontUtils::is_bold(wx_font);
    if (is_font_bold) {
        // unset bold
        if (draw_clickable(IconType::bold, IconState::hovered,
                           IconType::unbold, IconState::hovered)) {
            boldness.reset();
            if (wx_font.GetWeight() != wxFontWeight::wxFONTWEIGHT_NORMAL) {
                wxFont new_wx_font = wx_font; // copy
                new_wx_font.SetWeight(wxFontWeight::wxFONTWEIGHT_NORMAL);
                if(!m_style_manager.set_wx_font(new_wx_font))
                    return false;
            }
            return true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Unset bold").c_str());
    } else {
        // set bold
        if (draw_button(IconType::bold)) {
            wxFont new_wx_font = wx_font; // copy
            auto new_ff = WxFontUtils::set_bold(new_wx_font, *ff.font_file);
            if (new_ff != nullptr) {
                if(!m_style_manager.set_wx_font(new_wx_font, std::move(new_ff)))
                    return false;
            } else {
                // bold font can't be loaded
                // set up boldness
                boldness = 20.f;
                //font_file->cache.empty();
            }
            return true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Set bold").c_str());
    }
    return false;
}

template<typename T> bool exist_change(const T &value, const T *default_value){
    if (default_value == nullptr) return false;
    return (value != *default_value);
}

template<> bool exist_change(const std::optional<float> &value, const std::optional<float> *default_value){
    if (default_value == nullptr) return false;
    return !is_approx(value, *default_value);
}

template<> bool exist_change(const float &value, const float *default_value){
    if (default_value == nullptr) return false;
    return !is_approx(value, *default_value);
}

template<typename T, typename Draw>
bool GLGizmoEmboss::revertible(const std::string &name,
                               T                 &value,
                               const T           *default_value,
                               const std::string &undo_tooltip,
                               float              undo_offset,
                               Draw               draw)
{
    bool changed = exist_change(value, default_value);
    if (changed || default_value == nullptr)
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, name);
    else
        ImGuiWrapper::text(name);

    bool result = draw();
    // render revert changes button
    if (changed) {        
        ImGui::SameLine(undo_offset);
        if (draw_button(IconType::undo)) {
            value = *default_value;
            return true;
        } else if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", undo_tooltip.c_str());
    }
    return result;
}


bool GLGizmoEmboss::rev_input(const std::string  &name,
                              float              &value,
                              const float       *default_value,
                              const std::string  &undo_tooltip,
                              float               step,
                              float               step_fast,
                              const char         *format,
                              ImGuiInputTextFlags flags)
{
    // draw offseted input
    auto draw_offseted_input = [&]()->bool{
        float input_offset = m_gui_cfg->input_offset;
        float input_width  = m_gui_cfg->input_width;
        ImGui::SameLine(input_offset);
        ImGui::SetNextItemWidth(input_width);
        return ImGui::InputFloat(("##" + name).c_str(),
            &value, step, step_fast, format, flags);
    };
    float undo_offset = ImGui::GetStyle().FramePadding.x;
    return revertible(name, value, default_value, undo_tooltip, undo_offset, draw_offseted_input);
}

bool GLGizmoEmboss::rev_checkbox(const std::string &name,
                                 bool              &value,
                                 const bool        *default_value,
                                 const std::string &undo_tooltip)
{
    // draw offseted input
    auto draw_offseted_input = [&]() -> bool {
        ImGui::SameLine(m_gui_cfg->advanced_input_offset);
        return ImGui::Checkbox(("##" + name).c_str(), &value);
    };
    float undo_offset  = ImGui::GetStyle().FramePadding.x;
    return revertible(name, value, default_value, undo_tooltip,
                      undo_offset, draw_offseted_input);
}

void GLGizmoEmboss::draw_style_edit() {
    const GuiCfg::Translations &tr = m_gui_cfg->translations;

    const std::optional<wxFont> &wx_font_opt = m_style_manager.get_wx_font();
    EmbossStyle &style = m_style_manager.get_style();

    assert(wx_font_opt.has_value());
    if (!wx_font_opt.has_value()) {
        ImGui::TextColored(ImGuiWrapper::COL_ORANGE_DARK, "%s", _u8L("WxFont is not loaded properly.").c_str());
        return;
    }

    bool exist_stored_style = m_style_manager.exist_stored_style();
    bool is_font_changed = false;
    if (exist_stored_style && wx_font_opt.has_value()) {        
        const wxFont &wx_font = *wx_font_opt;
        const EmbossStyle *stored_style = m_style_manager.get_stored_style();
        assert(stored_style != nullptr);
        const std::optional<wxFont> &stored_wx = m_style_manager.get_stored_wx_font();
        assert(stored_wx.has_value());
        bool is_font_face_changed = stored_wx->GetFaceName() != wx_font.GetFaceName();

        const std::optional<float> &skew = m_style_manager.get_font_prop().skew;
        bool is_italic = skew.has_value() || WxFontUtils::is_italic(wx_font);
        const std::optional<float> &skew_stored = stored_style->prop.skew;
        bool is_stored_italic = skew_stored.has_value() || WxFontUtils::is_italic(*stored_wx);
        bool is_italic_changed = is_italic != is_stored_italic;

        const std::optional<float> &boldness = m_style_manager.get_font_prop().boldness;
        bool is_bold = boldness.has_value() || WxFontUtils::is_bold(wx_font);
        const std::optional<float> &boldness_stored = stored_style->prop.boldness;
        bool is_stored_bold = boldness_stored.has_value() || WxFontUtils::is_bold(*stored_wx);
        bool is_bold_changed = is_bold != is_stored_bold;

        bool is_font_style_changed = is_italic_changed || is_bold_changed;

        is_font_changed = is_font_face_changed || is_font_style_changed;
    }

    if (is_font_changed || !exist_stored_style)
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, tr.font);
    else
        ImGuiWrapper::text(tr.font);
    ImGui::SameLine(m_gui_cfg->input_offset);
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);
    draw_font_list();
    ImGui::SameLine();
    bool exist_change = false;
    if (draw_italic_button()) exist_change = true;

    ImGui::SameLine();
    if (draw_bold_button()) exist_change = true;
    
    if (is_font_changed) {
        ImGui::SameLine(ImGui::GetStyle().FramePadding.x);
        if (draw_button(IconType::undo)) {
            const EmbossStyle *stored_style = m_style_manager.get_stored_style();
            style.path = stored_style->path;
            style.prop.boldness = stored_style->prop.boldness;
            style.prop.skew = stored_style->prop.skew;

            wxFont new_wx_font = WxFontUtils::load_wxFont(style.path);
            if (new_wx_font.IsOk() && 
                m_style_manager.set_wx_font(new_wx_font))
                exist_change = true;
        } else if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Revert font changes.").c_str());
    }

    if (exist_change) {
        m_style_manager.clear_glyphs_cache();
        process();
    }

    bool use_inch = wxGetApp().app_config->get("use_inches") == "1";
    const std::string revert_text_size = _u8L("Revert text size.");
    FontProp &font_prop = style.prop;
    const float * def_size = exist_stored_style? 
        &m_style_manager.get_stored_style()->prop.size_in_mm : nullptr;
    bool is_size_changed = false;
    if (use_inch) {
        float size_in_inch = ObjectManipulation::mm_to_in * font_prop.size_in_mm;
        float def_size_inch = exist_stored_style ? ObjectManipulation::mm_to_in * (*def_size) : 0.f;
        if (def_size != nullptr) def_size = &def_size_inch;
        if (rev_input(tr.size, size_in_inch, def_size, revert_text_size, 0.1f, 1.f, "%.2f in")) { 
            font_prop.size_in_mm = ObjectManipulation::in_to_mm * size_in_inch;
            is_size_changed = true;
        }
    } else {
        if (rev_input(tr.size, font_prop.size_in_mm, def_size, revert_text_size, 0.1f, 1.f, "%.1f mm"))
            is_size_changed = true;
    }

    if (is_size_changed) {
        // size can't be zero or negative
        Limits::apply(font_prop.size_in_mm, limits.size_in_mm);

        // only different value need process
        if (!is_approx(font_prop.size_in_mm, m_volume->text_configuration->style.prop.size_in_mm)) {
            // store font size into path
            if (style.type == WxFontUtils::get_actual_type()) {
                if (wx_font_opt.has_value()) {
                    wxFont wx_font = *wx_font_opt;
                    wx_font.SetPointSize(static_cast<int>(font_prop.size_in_mm));
                    m_style_manager.set_wx_font(wx_font);
                }
            }
            process();
        }
    }

#ifdef SHOW_WX_WEIGHT_INPUT
    if (wx_font.has_value()) {
        ImGui::Text("%s", "weight");
        ImGui::SameLine(m_gui_cfg->input_offset);
        ImGui::SetNextItemWidth(m_gui_cfg->input_width);
        int weight     = wx_font->GetNumericWeight();
        int min_weight = 1, max_weight = 1000;
        if (ImGui::SliderInt("##weight", &weight, min_weight, max_weight)) {
            wx_font->SetNumericWeight(weight);
            m_style_manager.wx_font_changed();
            process();
        }

        wxFont f       = wx_font->Bold();
        bool   disable = f == *wx_font;
        ImGui::SameLine();
        if (draw_button(IconType::bold, disable)) {
            *wx_font = f;
            m_style_manager.wx_font_changed();
            process();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("wx Make bold").c_str());
    }
#endif // SHOW_WX_WEIGHT_INPUT

    const std::string revert_emboss_depth = _u8L("Revert embossed depth.");
    const float *def_depth = exist_stored_style ?
        &m_style_manager.get_stored_style()->prop.emboss : nullptr;
    bool is_depth_changed = false;
    if (use_inch) {
        float depthj_in_inch = ObjectManipulation::mm_to_in * font_prop.emboss;
        float def_depth_inch = exist_stored_style ? ObjectManipulation::mm_to_in * (*def_depth) : 0.f;
        if (def_depth != nullptr) def_depth = &def_depth_inch;
        if (rev_input(tr.depth, depthj_in_inch, def_depth, revert_emboss_depth, 0.1f, 0.25, "%.3f in")) { 
            font_prop.emboss = ObjectManipulation::in_to_mm * depthj_in_inch;
            is_depth_changed = true;
        }
    } else {
        if (rev_input(tr.depth, font_prop.emboss, def_depth, revert_emboss_depth, 0.1f, 0.25, "%.2f mm"))
            is_depth_changed = true;
    }

    if (is_depth_changed) {
        Limits::apply(font_prop.emboss, limits.emboss);
        process();
    }    
}

bool GLGizmoEmboss::rev_slider(const std::string &name,
                               std::optional<int>& value,
                               const std::optional<int> *default_value,
                               const std::string &undo_tooltip,
                               int                v_min,
                               int                v_max,
                               const std::string& format,
                               const wxString    &tooltip)
{    
    auto draw_slider_optional_int = [&]() -> bool {
        float slider_offset = m_gui_cfg->advanced_input_offset;
        float slider_width  = m_gui_cfg->input_width;
        ImGui::SameLine(slider_offset);
        ImGui::SetNextItemWidth(slider_width);
        return m_imgui->slider_optional_int( ("##" + name).c_str(), value, 
            v_min, v_max, format.c_str(), 1.f, false, tooltip);
    };
    float undo_offset = ImGui::GetStyle().FramePadding.x;
    return revertible(name, value, default_value,
        undo_tooltip, undo_offset, draw_slider_optional_int);
}

bool GLGizmoEmboss::rev_slider(const std::string &name,
                               std::optional<float>& value,
                               const std::optional<float> *default_value,
                               const std::string &undo_tooltip,
                               float                v_min,
                               float                v_max,
                               const std::string& format,
                               const wxString    &tooltip)
{    
    auto draw_slider_optional_float = [&]() -> bool {
        float slider_offset = m_gui_cfg->advanced_input_offset;
        float slider_width  = m_gui_cfg->input_width;
        ImGui::SameLine(slider_offset);
        ImGui::SetNextItemWidth(slider_width);
        return m_imgui->slider_optional_float(("##" + name).c_str(), value,
            v_min, v_max, format.c_str(), 1.f, false, tooltip);
    };
    float undo_offset = ImGui::GetStyle().FramePadding.x;
    return revertible(name, value, default_value,
        undo_tooltip, undo_offset, draw_slider_optional_float);
}

bool GLGizmoEmboss::rev_slider(const std::string &name,
                               float             &value,
                               const float       *default_value,
                               const std::string &undo_tooltip,
                               float              v_min,
                               float              v_max,
                               const std::string &format,
                               const wxString    &tooltip)
{    
    auto draw_slider_float = [&]() -> bool {
        float slider_offset = m_gui_cfg->advanced_input_offset;
        float slider_width  = m_gui_cfg->input_width;
        ImGui::SameLine(slider_offset);
        ImGui::SetNextItemWidth(slider_width);
        return m_imgui->slider_float("##" + name, &value, v_min, v_max,
            format.c_str(), 1.f, false, tooltip);
    };
    float undo_offset = ImGui::GetStyle().FramePadding.x;
    return revertible(name, value, default_value,
        undo_tooltip, undo_offset, draw_slider_float);
}

void GLGizmoEmboss::do_translate(const Vec3d &relative_move)
{
    assert(m_volume != nullptr);
    assert(m_volume->text_configuration.has_value());
    Selection &selection = m_parent.get_selection();
    assert(!selection.is_empty());
    selection.setup_cache();
    selection.translate(relative_move, TransformationType::Local);

    std::string snapshot_name; // empty meand no store undo / redo
    // NOTE: it use L instead of _L macro because prefix _ is appended inside
    // function do_move
    // snapshot_name = L("Set surface distance");
    m_parent.do_move(snapshot_name);
}

void GLGizmoEmboss::do_rotate(float relative_z_angle)
{
    assert(m_volume != nullptr);
    assert(m_volume->text_configuration.has_value());
    Selection &selection = m_parent.get_selection();
    assert(!selection.is_empty());
    selection.setup_cache();
    TransformationType transformation_type = TransformationType::Local_Relative_Joint;
    selection.rotate(Vec3d(0., 0., relative_z_angle), transformation_type);

    std::string snapshot_name; // empty meand no store undo / redo
    // NOTE: it use L instead of _L macro because prefix _ is appended
    // inside function do_move
    // snapshot_name = L("Set text rotation");
    m_parent.do_rotate(snapshot_name);
}

void GLGizmoEmboss::set_fine_position()
{
    const Selection &selection = m_parent.get_selection();
    const Selection::IndicesList indices   = selection.get_volume_idxs();
    // no selected volume
    if (indices.empty()) return;
    const GLVolume *volume = selection.get_volume(*indices.begin());
    // bad volume selected (e.g. deleted one)
    if (volume == nullptr) return;

    const Camera &camera = wxGetApp().plater()->get_camera();
    Polygon hull = CameraUtils::create_hull2d(camera, *volume);

    const ImVec2 &windows_size = get_minimal_window_size();
    Size          c_size       = m_parent.get_canvas_size();
    ImVec2 canvas_size(c_size.get_width(), c_size.get_height());
    ImVec2 offset = ImGuiWrapper::suggest_location(windows_size, hull, canvas_size);
    m_set_window_offset = offset;
    return;

    Polygon rect({Point(offset.x, offset.y),
                  Point(offset.x + windows_size.x, offset.y),
                  Point(offset.x + windows_size.x, offset.y + windows_size.y),
                  Point(offset.x, offset.y + windows_size.y)});
    ImGuiWrapper::draw(hull);
    ImGuiWrapper::draw(rect);
}

void GLGizmoEmboss::draw_advanced()
{
    const auto &ff = m_style_manager.get_font_file_with_cache();
    if (!ff.has_value()) { 
        ImGui::Text("%s", _u8L("Advanced font options could be change only for corect font.\nStart with select correct font.").c_str());
        return;
    }

    FontProp &font_prop = m_style_manager.get_style().prop;
    const auto  &cn = m_style_manager.get_font_prop().collection_number;
    unsigned int font_index = (cn.has_value()) ? *cn : 0;
    const auto  &font_info  = ff.font_file->infos[font_index];

#ifdef SHOW_FONT_FILE_PROPERTY
    ImGui::SameLine();
    int cache_size = ff.has_value()? (int)ff.cache->size() : 0;
    std::string ff_property = 
        "ascent=" + std::to_string(font_info.ascent) +
        ", descent=" + std::to_string(font_info.descent) +
        ", lineGap=" + std::to_string(font_info.linegap) +
        ", unitPerEm=" + std::to_string(font_info.unit_per_em) + 
        ", cache(" + std::to_string(cache_size) + " glyphs)";
    if (font_file->infos.size() > 1) { 
        unsigned int collection = font_prop.collection_number.has_value() ?
            *font_prop.collection_number : 0;
        ff_property += ", collect=" + std::to_string(collection+1) + "/" + std::to_string(font_file->infos.size());
    }
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, ff_property);
#endif // SHOW_FONT_FILE_PROPERTY

    bool exist_change = false;
    auto &tr = m_gui_cfg->translations;

    const EmbossStyle *stored_style = nullptr;
    if (m_style_manager.exist_stored_style())
        stored_style = m_style_manager.get_stored_style();

    bool can_use_surface = (m_volume==nullptr)? false :
                        (font_prop.use_surface)? true : // already used surface must have option to uncheck
        (m_volume->get_object()->volumes.size() > 1);
    m_imgui->disabled_begin(!can_use_surface);
    const bool *def_use_surface = stored_style ?
        &stored_style->prop.use_surface : nullptr;
    if (rev_checkbox(tr.use_surface, font_prop.use_surface, def_use_surface,
                     _u8L("Revert using of model surface."))) {
        if (font_prop.use_surface) { 
            font_prop.distance.reset();
            if (font_prop.emboss < 0.1)
                font_prop.emboss = 1;
        }
        process();
    }
    m_imgui->disabled_end(); // !can_use_surface

    std::string units = _u8L("font points");
    std::string units_fmt = "%.0f " + units;
    
    // input gap between letters
    auto def_char_gap = stored_style ?
        &stored_style->prop.char_gap : nullptr;

    int half_ascent = font_info.ascent / 2;
    int min_char_gap = -half_ascent, max_char_gap = half_ascent;
    if (rev_slider(tr.char_gap, font_prop.char_gap, def_char_gap, _u8L("Revert gap between letters"), 
        min_char_gap, max_char_gap, units_fmt, _L("Distance between letters"))){
        // Condition prevent recalculation when insertint out of limits value by imgui input
        if (!Limits::apply(font_prop.char_gap, limits.char_gap) ||
            !m_volume->text_configuration->style.prop.char_gap.has_value() ||
            m_volume->text_configuration->style.prop.char_gap != font_prop.char_gap) {        
            // char gap is stored inside of imgui font atlas
            m_style_manager.clear_imgui_font();
            exist_change = true;
        }
    }

    // input gap between lines
    auto def_line_gap = stored_style ?
        &stored_style->prop.line_gap : nullptr;
    int  min_line_gap = -half_ascent, max_line_gap = half_ascent;
    if (rev_slider(tr.line_gap, font_prop.line_gap, def_line_gap, _u8L("Revert gap between lines"), 
        min_line_gap, max_line_gap, units_fmt, _L("Distance between lines"))){
        // Condition prevent recalculation when insertint out of limits value by imgui input
        if (!Limits::apply(font_prop.line_gap, limits.line_gap) ||
            !m_volume->text_configuration->style.prop.line_gap.has_value() ||
            m_volume->text_configuration->style.prop.line_gap != font_prop.line_gap) {        
            // line gap is planed to be stored inside of imgui font atlas
            m_style_manager.clear_imgui_font();
            exist_change = true;
        }
    }

    // input boldness
    auto def_boldness = stored_style ?
        &stored_style->prop.boldness : nullptr;
    if (rev_slider(tr.boldness, font_prop.boldness, def_boldness, _u8L("Undo boldness"), 
        limits.boldness.gui.min, limits.boldness.gui.max, units_fmt, _L("Tiny / Wide glyphs"))){
        if (!Limits::apply(font_prop.boldness, limits.boldness.values) ||
            !m_volume->text_configuration->style.prop.boldness.has_value() ||
            m_volume->text_configuration->style.prop.boldness != font_prop.boldness)
            exist_change = true;
    }

    // input italic
    auto def_skew = stored_style ?
        &stored_style->prop.skew : nullptr;
    if (rev_slider(tr.italic, font_prop.skew, def_skew, _u8L("Undo letter's skew"),
        limits.skew.gui.min, limits.skew.gui.max, "%.2f", _L("Italic strength ratio"))){
        if (!Limits::apply(font_prop.skew, limits.skew.values) ||
            !m_volume->text_configuration->style.prop.skew.has_value() ||
            m_volume->text_configuration->style.prop.skew != font_prop.skew)
            exist_change = true;
    }
    
    // input surface distance
    bool allowe_surface_distance = 
        !m_volume->text_configuration->style.prop.use_surface &&
        !is_text_object(m_volume);    
    std::optional<float> &distance = font_prop.distance;
    float prev_distance = distance.has_value() ? *distance : .0f,
          min_distance = -2 * font_prop.emboss,
          max_distance = 2 * font_prop.emboss;
    auto def_distance = stored_style ?
        &stored_style->prop.distance : nullptr;    
    m_imgui->disabled_begin(!allowe_surface_distance);
    
    bool use_inch = wxGetApp().app_config->get("use_inches") == "1";
    const std::string undo_move_tooltip = _u8L("Undo translation");
    const wxString move_tooltip = _L("Distance center of text from model surface");
    bool is_moved = false;
    if (use_inch) {
        std::optional<float> distance_inch;
        if (distance.has_value()) distance_inch = (*distance * ObjectManipulation::mm_to_in);
        std::optional<float> def_distance_inch;
        if (def_distance != nullptr) {
            if (def_distance->has_value()) def_distance_inch = ObjectManipulation::mm_to_in * (*(*def_distance));
            def_distance = &def_distance_inch;
        }
        min_distance *= ObjectManipulation::mm_to_in;
        max_distance *= ObjectManipulation::mm_to_in;
        if (rev_slider(tr.surface_distance, distance_inch, def_distance, undo_move_tooltip, min_distance, max_distance, "%.3f in", move_tooltip)) {
            if (distance_inch.has_value()) {
                font_prop.distance = *distance_inch * ObjectManipulation::in_to_mm;
            } else {
                font_prop.distance.reset();
            }
            is_moved = true;
        }
    } else {
        if (rev_slider(tr.surface_distance, distance, def_distance, undo_move_tooltip, 
        min_distance, max_distance, "%.2f mm", move_tooltip)) is_moved = true;
    }

    if (is_moved){
        m_volume->text_configuration->style.prop.distance = font_prop.distance;        
        float act_distance = font_prop.distance.has_value() ? *font_prop.distance : .0f;
        do_translate(Vec3d::UnitZ() * (act_distance - prev_distance));
    }
    m_imgui->disabled_end();

    // slider for Clock-wise angle in degress
    // stored angle is optional CCW and in radians
    std::optional<float> &angle = font_prop.angle;
    float prev_angle = angle.has_value() ? *angle : .0f;
    // Convert stored value to degress
    // minus create clock-wise roation from CCW
    float angle_deg = angle.has_value() ?
        static_cast<float>(-(*angle) * 180 / M_PI) : .0f;
    float def_angle_deg_val = 
        (!stored_style || !stored_style->prop.angle.has_value()) ?
        0.f : (*stored_style->prop.angle * -180 / M_PI);
    float* def_angle_deg = stored_style ?
        &def_angle_deg_val : nullptr;
    if (rev_slider(tr.angle, angle_deg, def_angle_deg, _u8L("Undo rotation"), 
        limits.angle.min, limits.angle.max, u8"%.2f °",
                   _L("Rotate text Clock-wise."))) {
        // convert back to radians and CCW
        angle = -angle_deg * M_PI / 180.0;
        to_range_pi_pi(*angle);
        if (is_approx(*angle, 0.f))
            angle.reset();
        
        m_volume->text_configuration->style.prop.angle = angle;
        float act_angle = angle.has_value() ? *angle : .0f;
        do_rotate(act_angle - prev_angle);
        // recalculate for surface cut
        if (font_prop.use_surface) process();
    }

    // when more collection add selector
    if (ff.font_file->infos.size() > 1) {
        ImGui::Text("%s", tr.collection.c_str());
        ImGui::SameLine(m_gui_cfg->advanced_input_offset);
        ImGui::SetNextItemWidth(m_gui_cfg->input_width);
        unsigned int selected = font_prop.collection_number.has_value() ?
                               *font_prop.collection_number : 0;
        if (ImGui::BeginCombo("## Font collection", std::to_string(selected).c_str())) {
            for (unsigned int i = 0; i < ff.font_file->infos.size(); ++i) {
                ImGui::PushID(1 << (10 + i));
                bool is_selected = (i == selected);
                if (ImGui::Selectable(std::to_string(i).c_str(), is_selected)) {
                    if (i == 0) font_prop.collection_number.reset();
                    else font_prop.collection_number = i;
                    exist_change = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", _u8L("Select from True Type Collection.").c_str());
        }
    }

    if (exist_change) {
        m_style_manager.clear_glyphs_cache();
        process();
    }
#ifdef ALLOW_DEBUG_MODE
    ImGui::Text("family = %s", (font_prop.family.has_value() ?
                                    font_prop.family->c_str() :
                                    " --- "));
    ImGui::Text("face name = %s", (font_prop.face_name.has_value() ?
                                       font_prop.face_name->c_str() :
                                       " --- "));
    ImGui::Text("style = %s",
                (font_prop.style.has_value() ? font_prop.style->c_str() :
                                                 " --- "));
    ImGui::Text("weight = %s", (font_prop.weight.has_value() ?
                                    font_prop.weight->c_str() :
                                    " --- "));

    std::string descriptor = style.path;
    ImGui::Text("descriptor = %s", descriptor.c_str());
#endif // ALLOW_DEBUG_MODE
}

void GLGizmoEmboss::set_minimal_window_size(bool is_advance_edit_style)
{
    ImVec2 window_size = ImGui::GetWindowSize();
    const ImVec2& min_win_size_prev = get_minimal_window_size();
    //ImVec2 diff(window_size.x - min_win_size_prev.x,
    //            window_size.y - min_win_size_prev.y);
    float diff_y = window_size.y - min_win_size_prev.y;
    m_is_advanced_edit_style = is_advance_edit_style;
    const ImVec2 &min_win_size = get_minimal_window_size();
    ImGui::SetWindowSize(ImVec2(0.f, min_win_size.y + diff_y),
                         ImGuiCond_Always);
}

const ImVec2 &GLGizmoEmboss::get_minimal_window_size() const
{
    return (!m_is_advanced_edit_style) ? m_gui_cfg->minimal_window_size :
        ((!m_style_manager.has_collections())? m_gui_cfg->minimal_window_size_with_advance :
            m_gui_cfg->minimal_window_size_with_collections);
}

#ifdef ALLOW_ADD_FONT_BY_OS_SELECTOR
bool GLGizmoEmboss::choose_font_by_wxdialog()
{
    wxFontData data;
    data.EnableEffects(false);
    data.RestrictSelection(wxFONTRESTRICT_SCALABLE);
    // set previous selected font
    EmbossStyle &selected_style = m_style_manager.get_style();
    if (selected_style.type == WxFontUtils::get_actual_type()) {
        std::optional<wxFont> selected_font = WxFontUtils::load_wxFont(
            selected_style.path);
        if (selected_font.has_value()) data.SetInitialFont(*selected_font);
    }

    wxFontDialog font_dialog(wxGetApp().mainframe, data);
    if (font_dialog.ShowModal() != wxID_OK) return false;

    data                = font_dialog.GetFontData();
    wxFont   wx_font       = data.GetChosenFont();
    size_t   font_index = m_style_manager.get_fonts().size();
    EmbossStyle emboss_style  = WxFontUtils::create_emboss_style(wx_font);

    // Check that deserialization NOT influence font
    // false - use direct selected wxFont in dialog
    // true - use font item (serialize and deserialize wxFont)
    bool use_deserialized_font = false;

    // Try load and use new added font
    if ((use_deserialized_font && !m_style_manager.load_style(font_index)) ||
        (!use_deserialized_font && !m_style_manager.load_style(emboss_style, wx_font))) {
        m_style_manager.erase(font_index);
        wxString message = GUI::format_wxstr(
            _L("Font '%1%' can't be used. Please select another."),
            emboss_style.name);
        wxString      title = _L("Selected font is NOT True-type.");
        MessageDialog not_loaded_font_message(nullptr, message, title, wxOK);
        not_loaded_font_message.ShowModal();
        return choose_font_by_wxdialog();
    }

    // fix dynamic creation of italic font
    const auto& cn = m_style_manager.get_font_prop().collection_number;
    unsigned int font_collection = cn.has_value() ? *cn : 0;
    const auto&ff = m_style_manager.get_font_file_with_cache();
    if (WxFontUtils::is_italic(wx_font) &&
        !Emboss::is_italic(*ff.font_file, font_collection)) {
        m_style_manager.get_style().prop.skew = 0.2;
    }
    return true;
}
#endif // ALLOW_ADD_FONT_BY_OS_SELECTOR

#ifdef ALLOW_ADD_FONT_BY_FILE
bool GLGizmoEmboss::choose_true_type_file()
{
    wxArrayString input_files;
    wxString      fontDir      = wxEmptyString;
    wxString      selectedFile = wxEmptyString;
    wxFileDialog  dialog(nullptr, _L("Choose one or more files (TTF, TTC):"),
                        fontDir, selectedFile, file_wildcards(FT_FONTS),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) dialog.GetPaths(input_files);
    if (input_files.IsEmpty()) return false;
    size_t index = m_style_manager.get_fonts().size();
    // use first valid font
    for (auto &input_file : input_files) {
        std::string path = std::string(input_file.c_str());
        std::string name = get_file_name(path);
        //make_unique_name(name, m_font_list);
        const FontProp& prop = m_style_manager.get_font_prop();
        EmbossStyle style{ name, path, EmbossStyle::Type::file_path, prop };
        m_style_manager.add_font(style);
        // set first valid added font as active
        if (m_style_manager.load_style(index)) return true;
        m_style_manager.erase(index);       
    }
    return false;
}
#endif // ALLOW_ADD_FONT_BY_FILE

bool GLGizmoEmboss::choose_svg_file()
{
    wxArrayString input_files;
    wxString      fontDir      = wxEmptyString;
    wxString      selectedFile = wxEmptyString;
    wxFileDialog  dialog(nullptr, _L("Choose SVG file:"), fontDir,
                        selectedFile, file_wildcards(FT_SVG),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) dialog.GetPaths(input_files);
    if (input_files.IsEmpty()) return false;
    if (input_files.size() != 1) return false;
    auto &      input_file = input_files.front();
    std::string path       = std::string(input_file.c_str());
    std::string name       = get_file_name(path);

    NSVGimage *image = nsvgParseFromFile(path.c_str(), "mm", 96.0f);
    ExPolygons polys = NSVGUtils::to_ExPolygons(image);
    nsvgDelete(image);

    BoundingBox bb;
    for (const auto &p : polys) bb.merge(p.contour.points);
    const FontProp &fp = m_style_manager.get_style().prop;
    float scale   = fp.size_in_mm / std::max(bb.max.x(), bb.max.y());
    auto  project = std::make_unique<ProjectScale>(
        std::make_unique<ProjectZ>(fp.emboss / scale), scale);
    indexed_triangle_set its = polygons2model(polys, *project);
    return false;
    // test store:
    // for (auto &poly : polys) poly.scale(1e5);
    // SVG svg("converted.svg", BoundingBox(polys.front().contour.points));
    // svg.draw(polys);
    //return add_volume(name, its);
}

void GLGizmoEmboss::create_notification_not_valid_font(
    const TextConfiguration &tc)
{
    // not neccessary, but for sure that old notification doesnt exist
    if (m_is_unknown_font) remove_notification_not_valid_font();
    m_is_unknown_font = true;

    auto type = NotificationType::UnknownFont;
    auto level =
        NotificationManager::NotificationLevel::WarningNotificationLevel;

    const EmbossStyle &es     = m_style_manager.get_style();
    const auto &origin_family = tc.style.prop.face_name;
    const auto &actual_family = es.prop.face_name;

    const std::string &origin_font_name = origin_family.has_value() ?
                                              *origin_family :
                                              tc.style.path;

    std::string actual_wx_face_name;
    if (!actual_family.has_value()) {
        auto& wx_font = m_style_manager.get_wx_font();
        if (wx_font.has_value()) {
            wxString    wx_face_name = wx_font->GetFaceName();
            actual_wx_face_name = std::string((const char *) wx_face_name.ToUTF8());
        }
    }

    const std::string &actual_font_name = actual_family.has_value() ? *actual_family : 
            (!actual_wx_face_name.empty() ? actual_wx_face_name : es.path);

    std::string text =
        GUI::format(_L("Can't load exactly same font(\"%1%\"), "
                       "Aplication select similar one(\"%2%\"). "
                       "You have to specify font for enable edit text."),
                    origin_font_name, actual_font_name);
    auto notification_manager = wxGetApp().plater()->get_notification_manager();
    notification_manager->push_notification(type, level, text);
}

void GLGizmoEmboss::remove_notification_not_valid_font()
{
    if (!m_is_unknown_font) return;
    m_is_unknown_font      = false;
    auto type                 = NotificationType::UnknownFont;
    auto notification_manager = wxGetApp().plater()->get_notification_manager();
    notification_manager->close_notification_of_type(type);
}

void GLGizmoEmboss::init_icons()
{
    // icon order has to match the enum IconType
    std::vector<std::string> filenames{
        "edit_button.svg",
        "delete.svg",
        "add_copies.svg", 
        "save.svg", 
        "undo.svg",    
        "make_italic.svg",
        "make_unitalic.svg",
        "make_bold.svg",
        "make_unbold.svg",   
        "search.svg",
        "open.svg",
        "add_text_part.svg",
        "add_text_negative.svg",
        "add_text_modifier.svg"
    };
    assert(filenames.size() == static_cast<size_t>(IconType::_count));
    std::string path = resources_dir() + "/icons/";
    for (std::string &filename : filenames) filename = path + filename;

    // state order has to match the enum IconState
    std::vector<std::pair<int, bool>> states;
    states.push_back(std::make_pair(1, false)); // Activable
    states.push_back(std::make_pair(0, false));  // Hovered
    states.push_back(std::make_pair(2, false)); // Disabled

    bool compress = false;
    bool is_loaded = m_icons_texture.load_from_svg_files_as_sprites_array(
        filenames, states, m_gui_cfg->icon_width, compress);

    if (!is_loaded ||
        (size_t)m_icons_texture.get_width() < (states.size() * m_gui_cfg->icon_width) ||
        (size_t)m_icons_texture.get_height() < (filenames.size() * m_gui_cfg->icon_width)) { 
        // bad load of icons, but all usage of m_icons_texture check that texture is initialized
        assert(false);
        m_icons_texture.reset();
    }
}

void GLGizmoEmboss::draw_icon(IconType icon, IconState state, ImVec2 size)
{
    // canot draw count
    assert(icon != IconType::_count);
    if (icon == IconType::_count) return;

    unsigned int icons_texture_id = m_icons_texture.get_id();
    int          tex_width        = m_icons_texture.get_width();
    int          tex_height       = m_icons_texture.get_height();
    // is icon loaded
    if ((icons_texture_id == 0) || (tex_width <= 1) || (tex_height <= 1)){
        ImGui::Text("▮");
        return;
    }
        
    int icon_width = m_gui_cfg->icon_width;
    ImTextureID tex_id = (void *) (intptr_t) (GLuint) icons_texture_id;
    int start_x = static_cast<unsigned>(state) * (icon_width + 1) + 1,
        start_y = static_cast<unsigned>(icon) * (icon_width + 1) + 1;

    ImVec2 uv0(start_x / (float) tex_width, 
               start_y / (float) tex_height);
    ImVec2 uv1((start_x + icon_width) / (float) tex_width,
               (start_y + icon_width) / (float) tex_height);
        
    if (size.x < 1 || size.y < 1)
        size = ImVec2(m_gui_cfg->icon_width, m_gui_cfg->icon_width);

    ImGui::Image(tex_id, size, uv0, uv1);
}

void GLGizmoEmboss::draw_transparent_icon()
{
    unsigned int icons_texture_id = m_icons_texture.get_id();
    int          tex_width        = m_icons_texture.get_width();
    int          tex_height       = m_icons_texture.get_height();
    // is icon loaded
    if ((icons_texture_id == 0) || (tex_width <= 1) || (tex_height <= 1)) {
        ImGui::Text("▯");
        return;
    }

    ImTextureID tex_id = (void *) (intptr_t) (GLuint) icons_texture_id;
    int    icon_width = m_gui_cfg->icon_width;
    ImVec2 icon_size(icon_width, icon_width);
    ImVec2 pixel_size(1.f / tex_width, 1.f / tex_height);
    // zero pixel is transparent in texture
    ImGui::Image(tex_id, icon_size, ImVec2(0, 0), pixel_size);
}

bool GLGizmoEmboss::draw_clickable(
    IconType icon, IconState state, 
    IconType hover_icon, IconState hover_state)
{
    // check of hover
    float cursor_x = ImGui::GetCursorPosX();
    draw_transparent_icon();
    ImGui::SameLine(cursor_x);

    if (ImGui::IsItemHovered()) {
        // redraw image
        draw_icon(hover_icon, hover_state);
    } else {
        // redraw normal image
        draw_icon(icon, state);
    }
    return ImGui::IsItemClicked();
}

bool GLGizmoEmboss::draw_button(IconType icon, bool disable)
{
    if (disable) {
        draw_icon(icon, IconState::disabled);
        return false;
    }
    return draw_clickable(
        icon, IconState::activable,
        icon, IconState::hovered
    );
}

bool GLGizmoEmboss::is_text_object(const ModelVolume *text) {
    if (text == nullptr) return false;
    if (!text->text_configuration.has_value()) return false;
    if (text->type() != ModelVolumeType::MODEL_PART) return false;
    for (const ModelVolume *v : text->get_object()->volumes) {
        if (v == text) continue;
        if (v->type() == ModelVolumeType::MODEL_PART) return false;
    }
    return true;
}

std::string GLGizmoEmboss::get_file_name(const std::string &file_path)
{
    size_t pos_last_delimiter = file_path.find_last_of("/\\");
    size_t pos_point          = file_path.find_last_of('.');
    size_t offset             = pos_last_delimiter + 1;
    size_t count              = pos_point - pos_last_delimiter - 1;
    return file_path.substr(offset, count);
}

/////////////
// priv namespace implementation
///////////////

DataBase priv::create_emboss_data_base(const std::string &text, StyleManager& style_manager)
{
    auto create_volume_name = [&]() {
        bool        contain_enter = text.find('\n') != std::string::npos;
        std::string text_fixed;
        if (contain_enter) {
            // change enters to space
            text_fixed = text; // copy
            std::replace(text_fixed.begin(), text_fixed.end(), '\n', ' ');
        }
        return _u8L("Text") + " - " + ((contain_enter) ? text_fixed : text);
    };

    auto create_configuration = [&]() -> TextConfiguration {
        if (!style_manager.is_activ_font()) {
            std::string       default_text_for_emboss = _u8L("Embossed text");
            EmbossStyle       es                      = style_manager.get_style();
            TextConfiguration tc{es, default_text_for_emboss};
            // TODO: investigate how to initialize
            return tc;
        }

        EmbossStyle &es = style_manager.get_style();
        // actualize font path - during changes in gui it could be corrupted
        // volume must store valid path
        assert(style_manager.get_wx_font().has_value());
        assert(es.path.compare(WxFontUtils::store_wxFont(*style_manager.get_wx_font())) == 0);
        // style.path = WxFontUtils::store_wxFont(*m_style_manager.get_wx_font());
        return TextConfiguration{es, text};
    };

    return Slic3r::GUI::Emboss::DataBase{style_manager.get_font_file_with_cache(), create_configuration(), create_volume_name()};
}


Transform3d priv::create_transformation_on_bed(const Vec2d &screen_coor, const Camera &camera, const std::vector<Vec2d> &bed_shape, double z)
{
    // Create new object
    // calculate X,Y offset position for lay on platter in place of
    // mouse click
    Vec2d bed_coor = CameraUtils::get_z0_position(camera, screen_coor);

    // check point is on build plate:
    Points bed_shape_;
    bed_shape_.reserve(bed_shape.size());
    for (const Vec2d &p : bed_shape) bed_shape_.emplace_back(p.cast<int>());
    Slic3r::Polygon bed(bed_shape_);
    if (!bed.contains(bed_coor.cast<int>()))
        // mouse pose is out of build plate so create object in center of plate
        bed_coor = bed.centroid().cast<double>();

    Vec3d offset(bed_coor.x(), bed_coor.y(), z);
    // offset -= m_result.center();
    Transform3d::TranslationType tt(offset.x(), offset.y(), offset.z());
    return Transform3d(tt);
}

void priv::start_create_object_job(DataBase &emboss_data, const Vec2d &coor)
{
    // start creation of new object
    Plater        *plater    = wxGetApp().plater();
    const Camera  &camera    = plater->get_camera();
    const Pointfs &bed_shape = plater->build_volume().bed_shape();

    // can't create new object with distance from surface
    FontProp &prop = emboss_data.text_configuration.style.prop;
    if (prop.distance.has_value()) prop.distance.reset();

    // can't create new object with using surface
    if (prop.use_surface)
        prop.use_surface = false;

    //    Transform3d volume_tr = priv::create_transformation_on_bed(mouse_pos, camera, bed_shape, prop.emboss / 2);
    DataCreateObject data{std::move(emboss_data), coor, camera, bed_shape};
    auto             job    = std::make_unique<CreateObjectJob>(std::move(data));
    Worker          &worker = plater->get_ui_job_worker();
    queue_job(worker, std::move(job));
}

void priv::start_create_volume_job(const ModelObject *object,
                                   const Transform3d  volume_trmat,
                                   DataBase          &emboss_data,
                                   ModelVolumeType    volume_type)
{
    bool &use_surface = emboss_data.text_configuration.style.prop.use_surface;
    std::unique_ptr<GUI::Job> job;
    if (use_surface) {
        // Model to cut surface from.
        SurfaceVolumeData::ModelSources sources = create_sources(object->volumes);
        if (sources.empty()) {
            use_surface = false;
        } else {
            bool is_outside = volume_type == ModelVolumeType::MODEL_PART;
            // check that there is not unexpected volume type
            assert(is_outside || volume_type == ModelVolumeType::NEGATIVE_VOLUME || volume_type == ModelVolumeType::PARAMETER_MODIFIER);
            CreateSurfaceVolumeData surface_data{std::move(emboss_data), volume_trmat, is_outside,
                                                 std::move(sources),     volume_type,  object->id()};
            job = std::make_unique<CreateSurfaceVolumeJob>(std::move(surface_data));
        }
    }
    if (!use_surface) {
        // create volume
        DataCreateVolume data{std::move(emboss_data), volume_type, object->id(), volume_trmat};
        job = std::make_unique<CreateVolumeJob>(std::move(data));
    }

    Plater *plater = wxGetApp().plater();
    Worker &worker = plater->get_ui_job_worker();
    queue_job(worker, std::move(job));
}

GLVolume * priv::get_hovered_gl_volume(const GLCanvas3D &canvas) {
    int hovered_id_signed = canvas.get_first_hover_volume_idx();
    if (hovered_id_signed < 0) return nullptr;

    size_t hovered_id = static_cast<size_t>(hovered_id_signed);
    const GLVolumePtrs &volumes = canvas.get_volumes().volumes;
    if (hovered_id >= volumes.size()) return nullptr;

    return volumes[hovered_id];
}

bool priv::start_create_volume_on_surface_job(
    DataBase &emboss_data, ModelVolumeType volume_type, const Vec2d &screen_coor, const GLVolume *gl_volume, RaycastManager &raycaster)
{
    if (gl_volume == nullptr) return false;
    Plater *plater = wxGetApp().plater();
    const ModelObjectPtrs &objects = plater->model().objects;

    int object_idx = gl_volume->object_idx();
    if (object_idx < 0 ||  object_idx >= objects.size()) return false;
    ModelObject *obj = objects[object_idx];
    size_t vol_id = obj->volumes[gl_volume->volume_idx()]->id().id;
    auto cond = RaycastManager::AllowVolumes({vol_id});
    raycaster.actualize(obj, &cond);

    const Camera &camera = plater->get_camera();
    std::optional<RaycastManager::Hit> hit = raycaster.unproject(screen_coor, camera);

    // context menu for add text could be open only by right click on an
    // object. After right click, object is selected and object_idx is set
    // also hit must exist. But there is options to add text by object list
    if (!hit.has_value()) return false;

    Transform3d hit_object_trmat = raycaster.get_transformation(hit->tr_key);
    Transform3d hit_instance_trmat = gl_volume->get_instance_transformation().get_matrix();

    // Create result volume transformation
    Transform3d     surface_trmat = create_transformation_onto_surface(hit->position, hit->normal);
    const FontProp &font_prop     = emboss_data.text_configuration.style.prop;
    apply_transformation(font_prop, surface_trmat);
    Transform3d volume_trmat = hit_instance_trmat.inverse() * hit_object_trmat * surface_trmat;
    start_create_volume_job(obj, volume_trmat, emboss_data, volume_type);
    return true;
}

void priv::find_closest_volume(const Selection       &selection,
                               const Vec2d           &screen_center,
                               const Camera          &camera,
                               const ModelObjectPtrs &objects,
                               Vec2d                 *closest_center,
                               const GLVolume       **closest_volume)
{
    assert(closest_center != nullptr);
    assert(closest_volume != nullptr);
    assert(*closest_volume == nullptr);
    const Selection::IndicesList &indices = selection.get_volume_idxs();
    assert(!indices.empty()); // no selected volume
    if (indices.empty()) return;

    double center_sq_distance = std::numeric_limits<double>::max();
    for (unsigned int id : indices) {
        const GLVolume *gl_volume = selection.get_volume(id);
        ModelVolume *volume = priv::get_model_volume(gl_volume, objects);
        if (!volume->is_model_part()) continue;        
        Slic3r::Polygon hull = CameraUtils::create_hull2d(camera, *gl_volume);
        Vec2d c = hull.centroid().cast<double>();
        Vec2d d = c - screen_center;
        bool is_bigger_x = std::fabs(d.x()) > std::fabs(d.y());
        if ((is_bigger_x && d.x() * d.x() > center_sq_distance) ||
           (!is_bigger_x && d.y() * d.y() > center_sq_distance)) continue;

        double distance = d.squaredNorm();
        if (center_sq_distance < distance) continue;
        center_sq_distance = distance;
        *closest_center = c;
        *closest_volume = gl_volume;
    }
}

// any existing icon filename to not influence GUI
const std::string GLGizmoEmboss::M_ICON_FILENAME = "cut.svg";