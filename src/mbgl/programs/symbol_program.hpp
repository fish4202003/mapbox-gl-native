#pragma once

#include <mbgl/gl/context.hpp>
#include <mbgl/gl/program.hpp>
#include <mbgl/math/clamp.hpp>
#include <mbgl/util/interpolate.hpp>

#include <mbgl/programs/attributes.hpp>
#include <mbgl/programs/uniforms.hpp>
#include <mbgl/programs/segment.hpp>
#include <mbgl/shaders/symbol_icon.hpp>
#include <mbgl/shaders/symbol_sdf.hpp>
#include <mbgl/util/geometry.hpp>
#include <mbgl/util/size.hpp>
#include <mbgl/style/layers/symbol_layer_properties.hpp>
#include <mbgl/style/layers/symbol_layer_impl.hpp>
#include <mbgl/renderer/layers/render_symbol_layer.hpp>


#include <cmath>
#include <array>

namespace mbgl {

namespace style {
class SymbolPropertyValues;
} // namespace style

class RenderTile;
class TransformState;

namespace uniforms {
MBGL_DEFINE_UNIFORM_MATRIX(double, 4, u_gl_coord_matrix);
MBGL_DEFINE_UNIFORM_MATRIX(double, 4, u_label_plane_matrix);
MBGL_DEFINE_UNIFORM_SCALAR(gl::TextureUnit, u_texture);
MBGL_DEFINE_UNIFORM_SCALAR(bool, u_is_halo);
MBGL_DEFINE_UNIFORM_SCALAR(float, u_gamma_scale);

MBGL_DEFINE_UNIFORM_SCALAR(bool, u_is_text);
MBGL_DEFINE_UNIFORM_SCALAR(bool, u_is_size_zoom_constant);
MBGL_DEFINE_UNIFORM_SCALAR(bool, u_is_size_feature_constant);
MBGL_DEFINE_UNIFORM_SCALAR(float, u_size_t);
MBGL_DEFINE_UNIFORM_SCALAR(float, u_size);
MBGL_DEFINE_UNIFORM_SCALAR(float, u_max_camera_distance);
MBGL_DEFINE_UNIFORM_SCALAR(bool, u_rotate_symbol);
MBGL_DEFINE_UNIFORM_SCALAR(float, u_aspect_ratio);
} // namespace uniforms

struct SymbolLayoutAttributes : gl::Attributes<
    attributes::a_pos_offset,
    attributes::a_data<uint16_t, 4>>
{
    static Vertex vertex(Point<float> labelAnchor,
                         Point<float> o,
                         float glyphOffsetY,
                         uint16_t tx,
                         uint16_t ty,
                         const Range<float>& sizeData) {
        return Vertex {
            // combining pos and offset to reduce number of vertex attributes passed to shader (8 max for some devices)
            {{
                static_cast<int16_t>(labelAnchor.x),
                static_cast<int16_t>(labelAnchor.y),
                static_cast<int16_t>(::round(o.x * 64)),  // use 1/64 pixels for placement
                static_cast<int16_t>(::round((o.y + glyphOffsetY) * 64))
            }},
            {{
                tx,
                ty,
                static_cast<uint16_t>(sizeData.min * 10),
                static_cast<uint16_t>(sizeData.max * 10)
            }}
        };
    }
};

struct SymbolDynamicLayoutAttributes : gl::Attributes<attributes::a_projected_pos> {
    static Vertex vertex(Point<float> anchorPoint, float labelAngle, float labelminzoom) {
        return Vertex {
            {{
                 anchorPoint.x,
                 anchorPoint.y,
                 static_cast<float>(mbgl::attributes::packUint8Pair(
                         static_cast<uint8_t>(std::fmod(labelAngle + 2 * M_PI, 2 * M_PI) / (2 * M_PI) * 255),
                         static_cast<uint8_t>(labelminzoom * 10)))
             }}
        };
    }
};
    
struct ZoomEvaluatedSize {
    bool isZoomConstant;
    bool isFeatureConstant;
    float sizeT;
    float size;
    float layoutSize;
};
// Mimic the PaintPropertyBinder technique specifically for the {text,icon}-size layout properties
// in order to provide a 'custom' scheme for encoding the necessary attribute data.  As with
// PaintPropertyBinder, SymbolSizeBinder is an abstract class whose implementations handle the
// particular attribute & uniform logic needed by each possible type of the {Text,Icon}Size properties.
class SymbolSizeBinder {
public:
    virtual ~SymbolSizeBinder() = default;

    using Uniforms = gl::Uniforms<
        uniforms::u_is_size_zoom_constant,
        uniforms::u_is_size_feature_constant,
        uniforms::u_size_t,
        uniforms::u_size>;
    using UniformValues = Uniforms::Values;
    
    static std::unique_ptr<SymbolSizeBinder> create(const float tileZoom,
                                                    const style::DataDrivenPropertyValue<float>& sizeProperty,
                                                    const float defaultValue);

    virtual Range<float> getVertexSizeData(const GeometryTileFeature& feature) = 0;
    virtual ZoomEvaluatedSize evaluateForZoom(float currentZoom) const = 0;

    UniformValues uniformValues(float currentZoom) const {
        const ZoomEvaluatedSize u = evaluateForZoom(currentZoom);
        return UniformValues {
            uniforms::u_is_size_zoom_constant::Value{ u.isZoomConstant },
            uniforms::u_is_size_feature_constant::Value{ u.isFeatureConstant},
            uniforms::u_size_t::Value{ u.sizeT },
            uniforms::u_size::Value{ u.size }
        };
    }
};

// Return the smallest range of stops that covers the interval [lowerZoom, upperZoom]
template <class Stops>
Range<float> getCoveringStops(Stops s, float lowerZoom, float upperZoom) {
    assert(!s.stops.empty());
    auto minIt = s.stops.lower_bound(lowerZoom);
    auto maxIt = s.stops.lower_bound(upperZoom);
    
    // lower_bound yields first element >= lowerZoom, but we want the *last*
    // element <= lowerZoom, so if we found a stop > lowerZoom, back up by one.
    if (minIt != s.stops.begin() && minIt != s.stops.end() && minIt->first > lowerZoom) {
        minIt--;
    }
    return Range<float> {
        minIt == s.stops.end() ? s.stops.rbegin()->first : minIt->first,
        maxIt == s.stops.end() ? s.stops.rbegin()->first : maxIt->first
    };
}

class ConstantSymbolSizeBinder final : public SymbolSizeBinder {
public:
    ConstantSymbolSizeBinder(const float /*tileZoom*/, const float& size, const float /*defaultValue*/)
      : layoutSize(size) {}
    
    ConstantSymbolSizeBinder(const float /*tileZoom*/, const style::Undefined&, const float defaultValue)
      : layoutSize(defaultValue) {}
    
    ConstantSymbolSizeBinder(const float tileZoom, const style::CameraFunction<float>& function_, const float /*defaultValue*/)
      : layoutSize(function_.evaluate(tileZoom + 1)) {
        function_.stops.match(
            [&] (const style::ExponentialStops<float>& stops) {
                const auto& zoomLevels = getCoveringStops(stops, tileZoom, tileZoom + 1);
                coveringRanges = std::make_tuple(
                    zoomLevels,
                    Range<float> { function_.evaluate(zoomLevels.min), function_.evaluate(zoomLevels.max) }
                );
                functionInterpolationBase = stops.base;
            },
            [&] (const style::IntervalStops<float>&) {
                function = function_;
            }
        );
    }
    
    Range<float> getVertexSizeData(const GeometryTileFeature&) override { return { 0.0f, 0.0f }; };
    
    ZoomEvaluatedSize evaluateForZoom(float currentZoom) const override {
        float size = layoutSize;
        bool isZoomConstant = !(coveringRanges || function);
        if (coveringRanges) {
            // Even though we could get the exact value of the camera function
            // at z = currentZoom, we intentionally do not: instead, we interpolate
            // between the camera function values at a pair of zoom stops covering
            // [tileZoom, tileZoom + 1] in order to be consistent with this
            // restriction on composite functions.
            const Range<float>& zoomLevels = std::get<0>(*coveringRanges);
            const Range<float>& sizeLevels = std::get<1>(*coveringRanges);
            float t = util::clamp(
                util::interpolationFactor(*functionInterpolationBase, zoomLevels, currentZoom),
                0.0f, 1.0f
            );
            size = sizeLevels.min + t * (sizeLevels.max - sizeLevels.min);
        } else if (function) {
            size = function->evaluate(currentZoom);
        }

        const float unused = 0.0f;
        return { isZoomConstant, true, unused, size, layoutSize };
    }
    
    float layoutSize;
    // used for exponential functions
    optional<std::tuple<Range<float>, Range<float>>> coveringRanges;
    optional<float> functionInterpolationBase;
    // used for interval functions
    optional<style::CameraFunction<float>> function;
};

class SourceFunctionSymbolSizeBinder final : public SymbolSizeBinder {
public:
    using Vertex = gl::detail::Vertex<gl::Attribute<uint16_t, 1>>;
    using VertexVector = gl::VertexVector<Vertex>;
    using VertexBuffer = gl::VertexBuffer<Vertex>;
    
    SourceFunctionSymbolSizeBinder(const float /*tileZoom*/, const style::SourceFunction<float>& function_, const float defaultValue_)
        : function(function_),
          defaultValue(defaultValue_) {
    }

    Range<float> getVertexSizeData(const GeometryTileFeature& feature) override {
        const float size = function.evaluate(feature, defaultValue);
        return { size, size };
    };
    
    ZoomEvaluatedSize evaluateForZoom(float) const override {
        const float unused = 0.0f;
        return { true, false, unused, unused, unused };
    }
    
    const style::SourceFunction<float>& function;
    const float defaultValue;
};

class CompositeFunctionSymbolSizeBinder final : public SymbolSizeBinder {
public:
    
    CompositeFunctionSymbolSizeBinder(const float tileZoom, const style::CompositeFunction<float>& function_, const float defaultValue_)
        : function(function_),
          defaultValue(defaultValue_),
          layoutZoom(tileZoom + 1),
          coveringZoomStops(function.stops.match(
            [&] (const auto& stops) {
            return getCoveringStops(stops, tileZoom, tileZoom + 1); }))
    {}

    Range<float> getVertexSizeData(const GeometryTileFeature& feature) override {
        return {
            function.evaluate(coveringZoomStops.min, feature, defaultValue),
            function.evaluate(coveringZoomStops.max, feature, defaultValue)
        };
    };
    
    ZoomEvaluatedSize evaluateForZoom(float currentZoom) const override {
        float sizeInterpolationT = util::clamp(
            util::interpolationFactor(1.0f, coveringZoomStops, currentZoom),
            0.0f, 1.0f
        );

        const float unused = 0.0f;
        return { false, false, sizeInterpolationT, unused, unused };
    }
    
    const style::CompositeFunction<float>& function;
    const float defaultValue;
    float layoutZoom;
    Range<float> coveringZoomStops;
};


template <class Shaders,
          class Primitive,
          class LayoutAttrs,
          class Uniforms,
          class PaintProps>
class SymbolProgram {
public:
    using LayoutAttributes = LayoutAttrs;
    using LayoutVertex = typename LayoutAttributes::Vertex;
    
    using LayoutAndSizeAttributes = gl::ConcatenateAttributes<LayoutAttributes, SymbolDynamicLayoutAttributes>;

    using PaintProperties = PaintProps;
    using PaintPropertyBinders = typename PaintProperties::Binders;
    using PaintAttributes = typename PaintPropertyBinders::Attributes;
    using Attributes = gl::ConcatenateAttributes<LayoutAndSizeAttributes, PaintAttributes>;

    using UniformValues = typename Uniforms::Values;
    using SizeUniforms = typename SymbolSizeBinder::Uniforms;
    using PaintUniforms = typename PaintPropertyBinders::Uniforms;
    using AllUniforms = gl::ConcatenateUniforms<Uniforms, gl::ConcatenateUniforms<SizeUniforms, PaintUniforms>>;

    using ProgramType = gl::Program<Primitive, Attributes, AllUniforms>;

    ProgramType program;

    SymbolProgram(gl::Context& context, const ProgramParameters& programParameters)
        : program(ProgramType::createProgram(
            context,
            programParameters,
            Shaders::name,
            Shaders::vertexSource,
            Shaders::fragmentSource)) {
    }

    template <class DrawMode>
    void draw(gl::Context& context,
              DrawMode drawMode,
              gl::DepthMode depthMode,
              gl::StencilMode stencilMode,
              gl::ColorMode colorMode,
              const UniformValues& uniformValues,
              const gl::VertexBuffer<LayoutVertex>& layoutVertexBuffer,
              const gl::VertexBuffer<SymbolDynamicLayoutAttributes::Vertex>& dynamicLayoutVertexBuffer,
              const SymbolSizeBinder& symbolSizeBinder,
              const gl::IndexBuffer<DrawMode>& indexBuffer,
              const SegmentVector<Attributes>& segments,
              const PaintPropertyBinders& paintPropertyBinders,
              const typename PaintProperties::PossiblyEvaluated& currentProperties,
              float currentZoom,
              const std::string& layerID) {
        typename AllUniforms::Values allUniformValues = uniformValues
            .concat(symbolSizeBinder.uniformValues(currentZoom))
            .concat(paintPropertyBinders.uniformValues(currentZoom, currentProperties));

        typename Attributes::Bindings allAttributeBindings = LayoutAttributes::bindings(layoutVertexBuffer)
            .concat(SymbolDynamicLayoutAttributes::bindings(dynamicLayoutVertexBuffer))
            .concat(paintPropertyBinders.attributeBindings(currentProperties));

        for (auto& segment : segments) {
            auto vertexArrayIt = segment.vertexArrays.find(layerID);

            if (vertexArrayIt == segment.vertexArrays.end()) {
                vertexArrayIt = segment.vertexArrays.emplace(layerID, context.createVertexArray()).first;
            }

            program.draw(
                context,
                std::move(drawMode),
                std::move(depthMode),
                std::move(stencilMode),
                std::move(colorMode),
                allUniformValues,
                vertexArrayIt->second,
                Attributes::offsetBindings(allAttributeBindings, segment.vertexOffset),
                indexBuffer,
                segment.indexOffset,
                segment.indexLength);
        }
    }
};

class SymbolIconProgram : public SymbolProgram<
    shaders::symbol_icon,
    gl::Triangle,
    SymbolLayoutAttributes,
    gl::Uniforms<
        uniforms::u_matrix,
        uniforms::u_label_plane_matrix,
        uniforms::u_gl_coord_matrix,
        uniforms::u_extrude_scale,
        uniforms::u_texsize,
        uniforms::u_texture,
        uniforms::u_fadetexture,
        uniforms::u_is_text,
        uniforms::u_collision_y_stretch,
        uniforms::u_camera_to_center_distance,
        uniforms::u_pitch,
        uniforms::u_pitch_with_map,
        uniforms::u_max_camera_distance,
        uniforms::u_rotate_symbol,
        uniforms::u_aspect_ratio>,
    style::IconPaintProperties>
{
public:
    using SymbolProgram::SymbolProgram;

    static UniformValues uniformValues(const bool isText,
                                       const style::SymbolPropertyValues&,
                                       const Size& texsize,
                                       const std::array<float, 2>& pixelsToGLUnits,
                                       const bool alongLine,
                                       const RenderTile&,
                                       const TransformState&);
};

enum class SymbolSDFPart {
    Fill = 1,
    Halo = 0
};

template <class PaintProperties>
class SymbolSDFProgram : public SymbolProgram<
    shaders::symbol_sdf,
    gl::Triangle,
    SymbolLayoutAttributes,
    gl::Uniforms<
        uniforms::u_matrix,
        uniforms::u_label_plane_matrix,
        uniforms::u_gl_coord_matrix,
        uniforms::u_extrude_scale,
        uniforms::u_texsize,
        uniforms::u_texture,
        uniforms::u_fadetexture,
        uniforms::u_is_text,
        uniforms::u_collision_y_stretch,
        uniforms::u_camera_to_center_distance,
        uniforms::u_pitch,
        uniforms::u_pitch_with_map,
        uniforms::u_max_camera_distance,
        uniforms::u_rotate_symbol,
        uniforms::u_aspect_ratio,
        uniforms::u_gamma_scale,
        uniforms::u_is_halo>,
    PaintProperties>
{
public:
    using BaseProgram = SymbolProgram<shaders::symbol_sdf,
        gl::Triangle,
        SymbolLayoutAttributes,
        gl::Uniforms<
            uniforms::u_matrix,
            uniforms::u_label_plane_matrix,
            uniforms::u_gl_coord_matrix,
            uniforms::u_extrude_scale,
            uniforms::u_texsize,
            uniforms::u_texture,
            uniforms::u_fadetexture,
            uniforms::u_is_text,
            uniforms::u_collision_y_stretch,
            uniforms::u_camera_to_center_distance,
            uniforms::u_pitch,
            uniforms::u_pitch_with_map,            
            uniforms::u_max_camera_distance,
            uniforms::u_rotate_symbol,
            uniforms::u_aspect_ratio,
            uniforms::u_gamma_scale,
            uniforms::u_is_halo>,
        PaintProperties>;
    
    using UniformValues = typename BaseProgram::UniformValues;
    

    
    using BaseProgram::BaseProgram;

    static UniformValues uniformValues(const bool isText,
                                       const style::SymbolPropertyValues&,
                                       const Size& texsize,
                                       const std::array<float, 2>& pixelsToGLUnits,
                                       const bool alongLine,
                                       const RenderTile&,
                                       const TransformState&,
                                       const SymbolSDFPart);
};

using SymbolSDFIconProgram = SymbolSDFProgram<style::IconPaintProperties>;
using SymbolSDFTextProgram = SymbolSDFProgram<style::TextPaintProperties>;

using SymbolLayoutVertex = SymbolLayoutAttributes::Vertex;
using SymbolIconAttributes = SymbolIconProgram::Attributes;
using SymbolTextAttributes = SymbolSDFTextProgram::Attributes;

} // namespace mbgl
