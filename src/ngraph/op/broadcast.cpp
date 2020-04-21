//*****************************************************************************
// Copyright 2017-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include "ngraph/op/broadcast.hpp"
#include "ngraph/attribute_visitor.hpp"
#include "ngraph/op/concat.hpp"
#include "ngraph/op/constant.hpp"
#include "ngraph/op/sum.hpp"
#include "ngraph/partial_shape.hpp"

#include <numeric>

using namespace std;
using namespace ngraph;

constexpr NodeTypeInfo op::v3::Broadcast::type_info;

op::v3::Broadcast::Broadcast(const Output<Node>& arg,
                             const Output<Node>& target_shape,
                             const Output<Node>& axes_mapping,
                             const AutoBroadcastSpec& broadcast_spec)
    : Op({arg, target_shape, axes_mapping})
    , m_broadcast_spec(broadcast_spec)
{
    NODE_VALIDATION_CHECK(this,
                          m_broadcast_spec.m_type == AutoBroadcastType::NONE,
                          "axes_mapping input should not be provided for mode other than explicit");

    constructor_validate_and_infer_types();
}

op::v3::Broadcast::Broadcast(const Output<Node>& arg,
                             const Output<Node>& target_shape,
                             const AutoBroadcastSpec& broadcast_spec)
    : Op({arg, target_shape, op::v0::Constant::create(element::u8, Shape{}, {0})->output(0)})
    , m_broadcast_spec(broadcast_spec)
{
    NODE_VALIDATION_CHECK(this,
                          m_broadcast_spec.m_type != AutoBroadcastType::NONE,
                          "axes_mapping input should be provided if explicit mode is used");

    constructor_validate_and_infer_types();
}

bool op::v3::Broadcast::visit_attributes(AttributeVisitor& visitor)
{
    visitor.on_attribute("broadcast_spec", m_broadcast_spec);
    return true;
}

std::pair<bool, AxisSet> op::v3::Broadcast::get_broadcast_axes() const
{
    AxisSet broadcast_axes;
    bool axes_known = false;

    if (m_broadcast_spec.m_type == AutoBroadcastType::NONE)
    {
        const auto axes_mapping_constant =
            as_type_ptr<op::v0::Constant>(input_value(2).get_node_shared_ptr());
        if (get_input_partial_shape(1).is_static() && axes_mapping_constant)
        {
            auto target_shape = get_input_shape(1);
            NGRAPH_CHECK(target_shape.size() == 1);
            auto axes_mapping_val = axes_mapping_constant->get_axis_vector_val();

            std::vector<size_t> axes(target_shape[0]);
            std::iota(axes.begin(), axes.end(), 0);
            for (auto i = axes_mapping_val.rbegin(); i != axes_mapping_val.rend(); ++i)
            {
                axes.erase(axes.begin() + *i);
            }
            broadcast_axes.insert(axes.begin(), axes.end());
            axes_known = true;
        }
    }
    else if (m_broadcast_spec.m_type == AutoBroadcastType::NUMPY ||
             m_broadcast_spec.m_type == AutoBroadcastType::PDPD ||
             m_broadcast_spec.m_type == AutoBroadcastType::BIDIRECTIONAL)
    {
        if (get_input_partial_shape(0).is_static() && get_output_partial_shape(0).is_static())
        {
            auto arg_shape = get_input_shape(0);
            auto result_shape = get_output_shape(0);
            auto start_axis = (m_broadcast_spec.m_type == AutoBroadcastType::PDPD)
                                  ? m_broadcast_spec.m_axis
                                  : result_shape.size() - arg_shape.size();
            NGRAPH_CHECK(start_axis >= 0);
            for (size_t i = 0; i < result_shape.size(); i++)
            {
                if (i < start_axis || result_shape[i] != arg_shape[i - start_axis])
                {
                    broadcast_axes.insert(i);
                }
            }
            axes_known = true;
        }
    }
    else
    {
        throw ngraph_error("Unknown autobroadcast type");
    }

    return std::make_pair(axes_known, broadcast_axes);
}

void op::v3::Broadcast::validate_and_infer_types()
{
    // shape node should have integer data type. For now we only allow i64
    auto shape_et = get_input_element_type(1);
    NODE_VALIDATION_CHECK(this,
                          shape_et.is_integral_number(),
                          "Broadcast shape must be an integral number, but is: ",
                          shape_et);
    // shape node should produce a one dimensional shape.
    auto broadcast_shape_rank = get_input_partial_shape(1).rank();
    NODE_VALIDATION_CHECK(this,
                          broadcast_shape_rank.compatible(1),
                          "Broadcast shape rank must be 1, but has ",
                          broadcast_shape_rank);

    if (m_broadcast_spec.m_type == AutoBroadcastType::NONE)
    {
        // axes_mapping node should have integer data type. For now we only allow i64
        auto axes_et = get_input_element_type(2);
        NODE_VALIDATION_CHECK(this,
                              axes_et.is_integral_number(),
                              "Broadcast axes must be integral numbers, but are: ",
                              axes_et);
        // axes_mapping node should produce a one dimensional shape.
        auto axes_shape_rank = get_input_partial_shape(2).rank();
        NODE_VALIDATION_CHECK(this,
                              axes_shape_rank.compatible(1),
                              "Broadcast axes rank must be 1, but has ",
                              axes_shape_rank);
    }

    PartialShape result_shape{PartialShape::dynamic()};

    const auto shape_constant = as_type_ptr<op::v0::Constant>(input_value(1).get_node_shared_ptr());

    if (shape_constant)
    {
        result_shape = shape_constant->get_shape_val();
    }
    else if (auto concat = as_type_ptr<op::v0::Concat>(input_value(1).get_node_shared_ptr()))
    {
        auto concat_inputs = concat->inputs();

        if (concat->get_output_partial_shape(0).is_static() && concat->get_shape().size() == 1 &&
            concat_inputs.size() == shape_size(concat->get_shape()))
        {
            auto output_partial_shape = vector<Dimension>{};
            for (const auto& concat_input : concat_inputs)
            {
                auto source_node_ptr = concat_input.get_source_output().get_node_shared_ptr();
                if (auto source_const_ptr = as_type_ptr<op::v0::Constant>(source_node_ptr))
                {
                    output_partial_shape.push_back(source_const_ptr->get_axis_vector_val()[0]);
                }
                else
                {
                    output_partial_shape.push_back(Dimension::dynamic());
                }
            }
            result_shape = PartialShape(output_partial_shape);
        }
    }

    if (m_broadcast_spec.m_type == AutoBroadcastType::NONE)
    {
        // Validate axes_mapping
        if (get_input_partial_shape(0).is_static() && get_input_partial_shape(1).is_static() &&
            get_input_partial_shape(2).is_static())
        {
            auto arg_shape = get_input_shape(0);
            auto axes_shape = get_input_shape(2);

            // Rank(arg_shape) == shape_size(axes_mapping)
            NODE_VALIDATION_CHECK(this,
                                  shape_size(axes_shape) == arg_shape.size(),
                                  "Broadcast axes_mapping shape ",
                                  axes_shape,
                                  " doesn't match rank of input tensor ",
                                  arg_shape.size());

            if (shape_constant && input_value(2).get_node_shared_ptr()->is_constant())
            {
                auto target_shape = shape_constant->get_shape_val();
                auto axes_mapping_val =
                    as_type_ptr<op::v0::Constant>(input_value(2).get_node_shared_ptr())
                        ->get_axis_vector_val();
                // axes_mapping needs to be in sorted order
                NODE_VALIDATION_CHECK(
                    this,
                    std::is_sorted(axes_mapping_val.begin(), axes_mapping_val.end()),
                    "Broadcast doesn't permit transposes. axes_mapping ",
                    axes_mapping_val,
                    " not in sorted order");

                for (size_t i = 0; i < axes_mapping_val.size(); i++)
                {
                    NODE_VALIDATION_CHECK(this,
                                          axes_mapping_val[i] < target_shape.size(),
                                          "Broadcast axes_mapping[",
                                          i,
                                          "]: ",
                                          axes_mapping_val[i],
                                          " exceeds target rank ",
                                          target_shape.size());

                    NODE_VALIDATION_CHECK(this,
                                          target_shape[axes_mapping_val[i]] == arg_shape[i],
                                          "Broadcast target[axes_mapping[",
                                          i,
                                          "]]",
                                          " Expected ",
                                          arg_shape[i],
                                          ". Got ",
                                          target_shape[axes_mapping_val[i]]);
                }
            }
        }
    }
    else if (m_broadcast_spec.m_type == AutoBroadcastType::NUMPY ||
             m_broadcast_spec.m_type == AutoBroadcastType::PDPD ||
             m_broadcast_spec.m_type == AutoBroadcastType::BIDIRECTIONAL)
    {
        if (get_input_partial_shape(0).is_static() && get_input_partial_shape(1).is_static())
        {
            auto arg_shape = get_input_shape(0);

            if (shape_constant)
            {
                auto target_shape = shape_constant->get_shape_val();

                if (m_broadcast_spec.m_type == AutoBroadcastType::BIDIRECTIONAL)
                {
                    // Add left padding to shorter target or argument shape
                    const auto target_padded_rank = std::max(arg_shape.size(), target_shape.size());
                    while (arg_shape.size() < target_padded_rank)
                    {
                        arg_shape.insert(arg_shape.begin(), 1);
                    }
                    while (target_shape.size() < target_padded_rank)
                    {
                        target_shape.insert(target_shape.begin(), 1);
                    }
                    result_shape = target_shape;
                }

                auto start_axis = (m_broadcast_spec.m_type == AutoBroadcastType::PDPD)
                                      ? m_broadcast_spec.m_axis
                                      : target_shape.size() - arg_shape.size();
                NODE_VALIDATION_CHECK(this,
                                      start_axis >= 0,
                                      "Broadcast target_shape has smaller rank ",
                                      target_shape.size(),
                                      " than arg shape ",
                                      arg_shape.size());
                for (auto i = start_axis; i < target_shape.size(); i++)
                {
                    NODE_VALIDATION_CHECK(
                        this,
                        arg_shape[i - start_axis] == 1 || target_shape[i] == 1 ||
                            arg_shape[i - start_axis] == target_shape[i],
                        "Broadcast incorrect target shape. Expecting either 1 or ",
                        arg_shape[i - start_axis],
                        ". Got ",
                        target_shape[i]);
                    result_shape[i] = std::max(arg_shape[i - start_axis], target_shape[i]);
                }
            }
        }
    }

    set_input_is_relevant_to_shape(0); // arg - Result element type
    set_input_is_relevant_to_shape(1); // target_shape - Result shape
    set_input_is_relevant_to_shape(2); // axes_mapping - Broadcast type
    set_output_type(0, get_input_element_type(0), result_shape);
}

shared_ptr<Node> op::v3::Broadcast::clone_with_new_inputs(const OutputVector& new_args) const
{
    check_new_args_count(this, new_args);
    return make_shared<v3::Broadcast>(
        new_args.at(0), new_args.at(1), new_args.at(2), m_broadcast_spec);
}

void op::v3::Broadcast::generate_adjoints(autodiff::Adjoints& adjoints, const OutputVector& deltas)
{
    auto delta = deltas.at(0);

    auto x = input_value(0);

    auto broadcast_axes = get_broadcast_axes();
    if (broadcast_axes.first)
    {
        adjoints.add_delta(x, make_shared<op::Sum>(delta, broadcast_axes.second));
    }
    else
    {
        throw ngraph_error("Autodiff not supported on dynamic op variants");
    }
}

constexpr NodeTypeInfo op::v1::Broadcast::type_info;

op::v1::Broadcast::Broadcast(const Output<Node>& arg,
                             const Output<Node>& target_shape,
                             const Output<Node>& axes_mapping,
                             const AutoBroadcastSpec& broadcast_spec)
    : Op({arg, target_shape, axes_mapping})
    , m_broadcast_spec(broadcast_spec)
{
    constructor_validate_and_infer_types();
}

op::v1::Broadcast::Broadcast(const Output<Node>& arg,
                             const Output<Node>& target_shape,
                             const AutoBroadcastSpec& broadcast_spec)
    : Op({arg, target_shape, op::v0::Constant::create(element::u8, Shape{}, {0})->output(0)})
    , m_broadcast_spec(broadcast_spec)
{
    constructor_validate_and_infer_types();
}

bool op::v1::Broadcast::visit_attributes(AttributeVisitor& visitor)
{
    visitor.on_attribute("broadcast_spec", m_broadcast_spec);
    return true;
}

std::pair<bool, AxisSet> op::v1::Broadcast::get_broadcast_axes() const
{
    AxisSet broadcast_axes;
    bool axes_known = false;

    if (m_broadcast_spec.m_type == AutoBroadcastType::NONE)
    {
        const auto axes_mapping_constant =
            as_type_ptr<op::v0::Constant>(input_value(2).get_node_shared_ptr());
        if (get_input_partial_shape(1).is_static() && axes_mapping_constant)
        {
            auto target_shape = get_input_shape(1);
            NGRAPH_CHECK(target_shape.size() == 1);
            auto axes_mapping_val = axes_mapping_constant->get_axis_vector_val();

            std::vector<size_t> axes(target_shape[0]);
            std::iota(axes.begin(), axes.end(), 0);
            for (auto i = axes_mapping_val.rbegin(); i != axes_mapping_val.rend(); ++i)
            {
                axes.erase(axes.begin() + *i);
            }
            broadcast_axes.insert(axes.begin(), axes.end());
            axes_known = true;
        }
    }
    else if (m_broadcast_spec.m_type == AutoBroadcastType::NUMPY ||
             m_broadcast_spec.m_type == AutoBroadcastType::PDPD)
    {
        if (get_input_partial_shape(0).is_static() && get_output_partial_shape(0).is_static())
        {
            auto arg_shape = get_input_shape(0);
            auto result_shape = get_output_shape(0);
            auto start_axis = (m_broadcast_spec.m_type == AutoBroadcastType::PDPD)
                                  ? m_broadcast_spec.m_axis
                                  : result_shape.size() - arg_shape.size();
            NGRAPH_CHECK(start_axis >= 0);
            for (size_t i = 0; i < result_shape.size(); i++)
            {
                if (i < start_axis || result_shape[i] != arg_shape[i - start_axis])
                {
                    broadcast_axes.insert(i);
                }
            }
            axes_known = true;
        }
    }
    else
    {
        throw ngraph_error("Unknown autobroadcast type");
    }

    return std::make_pair(axes_known, broadcast_axes);
}

void op::v1::Broadcast::validate_and_infer_types()
{
    // shape node should have integer data type. For now we only allow i64
    auto shape_et = get_input_element_type(1);
    NODE_VALIDATION_CHECK(this,
                          shape_et.is_integral_number(),
                          "Broadcast shape must be an integral number, but is: ",
                          shape_et);
    // shape node should produce a one dimensional shape.
    auto broadcast_shape_rank = get_input_partial_shape(1).rank();
    NODE_VALIDATION_CHECK(this,
                          broadcast_shape_rank.compatible(1),
                          "Broadcast shape rank must be 1, but has ",
                          broadcast_shape_rank);

    if (m_broadcast_spec.m_type == AutoBroadcastType::NONE)
    {
        // axes_mapping node should have integer data type. For now we only allow i64
        auto axes_et = get_input_element_type(2);
        NODE_VALIDATION_CHECK(this,
                              axes_et.is_integral_number(),
                              "Broadcast axes must be integral numbers, but are: ",
                              axes_et);
        // axes_mapping node should produce a one dimensional shape.
        auto axes_shape_rank = get_input_partial_shape(2).rank();
        NODE_VALIDATION_CHECK(this,
                              axes_shape_rank.compatible(1),
                              "Broadcast axes rank must be 1, but has ",
                              axes_shape_rank);
    }

    PartialShape result_shape{PartialShape::dynamic()};

    const auto shape_constant = as_type_ptr<op::v0::Constant>(input_value(1).get_node_shared_ptr());

    if (shape_constant)
    {
        result_shape = shape_constant->get_shape_val();
    }
    else if (auto concat = as_type_ptr<op::v0::Concat>(input_value(1).get_node_shared_ptr()))
    {
        auto concat_inputs = concat->inputs();

        if (concat->get_output_partial_shape(0).is_static() && concat->get_shape().size() == 1 &&
            concat_inputs.size() == shape_size(concat->get_shape()))
        {
            auto output_partial_shape = vector<Dimension>{};
            for (const auto& concat_input : concat_inputs)
            {
                auto source_node_ptr = concat_input.get_source_output().get_node_shared_ptr();
                if (auto source_const_ptr = as_type_ptr<op::v0::Constant>(source_node_ptr))
                {
                    output_partial_shape.push_back(source_const_ptr->get_axis_vector_val()[0]);
                }
                else
                {
                    output_partial_shape.push_back(Dimension::dynamic());
                }
            }
            result_shape = PartialShape(output_partial_shape);
        }
    }

    if (m_broadcast_spec.m_type == AutoBroadcastType::NONE)
    {
        // Validate axes_mapping
        if (get_input_partial_shape(0).is_static() && get_input_partial_shape(1).is_static() &&
            get_input_partial_shape(2).is_static())
        {
            auto arg_shape = get_input_shape(0);
            auto axes_shape = get_input_shape(2);

            // Rank(arg_shape) == shape_size(axes_mapping)
            NODE_VALIDATION_CHECK(this,
                                  shape_size(axes_shape) == arg_shape.size(),
                                  "Broadcast axes_mapping shape ",
                                  axes_shape,
                                  " doesn't match rank of input tensor ",
                                  arg_shape.size());

            if (shape_constant && input_value(2).get_node_shared_ptr()->is_constant())
            {
                auto target_shape = shape_constant->get_shape_val();
                auto axes_mapping_val =
                    as_type_ptr<op::v0::Constant>(input_value(2).get_node_shared_ptr())
                        ->get_axis_vector_val();
                // axes_mapping needs to be in sorted order
                NODE_VALIDATION_CHECK(
                    this,
                    std::is_sorted(axes_mapping_val.begin(), axes_mapping_val.end()),
                    "Broadcast doesn't permit transposes. axes_mapping ",
                    axes_mapping_val,
                    " not in sorted order");

                for (size_t i = 0; i < axes_mapping_val.size(); i++)
                {
                    NODE_VALIDATION_CHECK(this,
                                          axes_mapping_val[i] < target_shape.size(),
                                          "Broadcast axes_mapping[",
                                          i,
                                          "]: ",
                                          axes_mapping_val[i],
                                          " exceeds target rank ",
                                          target_shape.size());

                    NODE_VALIDATION_CHECK(this,
                                          target_shape[axes_mapping_val[i]] == arg_shape[i],
                                          "Broadcast target[axes_mapping[",
                                          i,
                                          "]]",
                                          " Expected ",
                                          arg_shape[i],
                                          ". Got ",
                                          target_shape[axes_mapping_val[i]]);
                }
            }
        }
    }
    else if (m_broadcast_spec.m_type == AutoBroadcastType::NUMPY ||
             m_broadcast_spec.m_type == AutoBroadcastType::PDPD)
    {
        if (get_input_partial_shape(0).is_static() && get_input_partial_shape(1).is_static())
        {
            auto arg_shape = get_input_shape(0);

            if (shape_constant)
            {
                const auto target_shape = shape_constant->get_shape_val();
                auto start_axis = (m_broadcast_spec.m_type == AutoBroadcastType::PDPD)
                                      ? m_broadcast_spec.m_axis
                                      : target_shape.size() - arg_shape.size();
                NODE_VALIDATION_CHECK(this,
                                      start_axis >= 0,
                                      "Broadcast target_shape has smaller rank ",
                                      target_shape.size(),
                                      " than arg shape ",
                                      arg_shape.size());
                for (auto i = start_axis; i < target_shape.size(); i++)
                {
                    NODE_VALIDATION_CHECK(
                        this,
                        arg_shape[i - start_axis] == 1 || target_shape[i] == 1 ||
                            arg_shape[i - start_axis] == target_shape[i],
                        "Broadcast incorrect target shape. Expecting either 1 or ",
                        arg_shape[i - start_axis],
                        " . Got ",
                        target_shape[i]);
                    result_shape[i] = std::max(arg_shape[i - start_axis], target_shape[i]);
                }
            }
        }
    }

    set_input_is_relevant_to_shape(0); // arg - Result element type
    set_input_is_relevant_to_shape(1); // target_shape - Result shape
    set_input_is_relevant_to_shape(2); // axes_mapping - Broadcast type
    set_output_type(0, get_input_element_type(0), result_shape);
}

shared_ptr<Node> op::v1::Broadcast::clone_with_new_inputs(const OutputVector& new_args) const
{
    check_new_args_count(this, new_args);
    return make_shared<v1::Broadcast>(
        new_args.at(0), new_args.at(1), new_args.at(2), m_broadcast_spec);
}

void op::v1::Broadcast::generate_adjoints(autodiff::Adjoints& adjoints, const OutputVector& deltas)
{
    auto delta = deltas.at(0);

    auto x = input_value(0);

    auto broadcast_axes = get_broadcast_axes();
    if (broadcast_axes.first)
    {
        adjoints.add_delta(x, make_shared<op::Sum>(delta, broadcast_axes.second));
    }
    else
    {
        throw ngraph_error("Autodiff not supported on dynamic op variants");
    }
}

constexpr NodeTypeInfo op::v0::Broadcast::type_info;

op::v0::Broadcast::Broadcast(const OutputVector& args,
                             const Shape& shape,
                             const AxisSet& broadcast_axes)
    : Op(args)
    , m_shape(shape)
    , m_broadcast_axes(broadcast_axes)
{
    constructor_validate_and_infer_types();
}

op::v0::Broadcast::Broadcast(const Output<Node>& arg,
                             const Shape& shape,
                             const AxisSet& broadcast_axes)
    : Broadcast(OutputVector{arg}, shape, broadcast_axes)
{
}

bool op::v0::Broadcast::visit_attributes(AttributeVisitor& visitor)
{
    visitor.on_attribute("shape", m_shape);
    visitor.on_attribute("broadcast_axes", m_broadcast_axes);
    return true;
}

void op::v0::Broadcast::validate_and_infer_types()
{
    infer_shape();

    for (auto axis : m_broadcast_axes)
    {
        NODE_VALIDATION_CHECK(this,
                              axis < m_shape.size(),
                              "Broadcast axis index (",
                              axis,
                              ") exceeds specified output shape rank ",
                              "(broadcast axes: ",
                              m_broadcast_axes,
                              ", output shape: ",
                              m_shape,
                              ").");
    }

    Shape required_input_shape = m_shape;
    for (auto i = m_broadcast_axes.rbegin(); i != m_broadcast_axes.rend(); ++i)
    {
        required_input_shape.erase(required_input_shape.begin() + *i);
    }

    // TODO(amprocte): We can probably have a more helpful error message here.
    // There are two things that can go wrong, which are being picked up in
    // one fell swoop by this check: either the number of broadcast axes is not
    // enough, or there is a mismatch with one of the pre-broadcast axis lengths.
    NODE_VALIDATION_CHECK(
        this,
        get_input_partial_shape(0).compatible(required_input_shape),
        "Broadcast argument shape, specified output shape, and axes are incompatible ",
        "(argument shape: ",
        get_input_partial_shape(0),
        ", output shape: ",
        m_shape,
        ", broadcast axes: ",
        m_broadcast_axes,
        ").");

    set_output_type(0, get_input_element_type(0), m_shape);
}

shared_ptr<Node> op::v0::Broadcast::clone_with_new_inputs(const OutputVector& new_args) const
{
    check_new_args_count(this, new_args);
    return make_shared<v0::Broadcast>(new_args.at(0), m_shape, m_broadcast_axes);
}

void op::v0::Broadcast::generate_adjoints(autodiff::Adjoints& adjoints, const OutputVector& deltas)
{
    auto delta = deltas.at(0);

    auto x = input_value(0);

    adjoints.add_delta(x, make_shared<op::Sum>(delta, m_broadcast_axes));
}

constexpr NodeTypeInfo op::v0::BroadcastLike::type_info;

op::v0::BroadcastLike::BroadcastLike(const Output<Node>& arg,
                                     const Output<Node>& like_arg,
                                     const AxisSet& initial_broadcast_axes)
    : op::v0::Broadcast({arg, like_arg}, {}, {})
    , m_initial_broadcast_axes(initial_broadcast_axes)
{
    constructor_validate_and_infer_types();
}

bool op::v0::BroadcastLike::visit_attributes(AttributeVisitor& visitor)
{
    visitor.on_attribute("shape", m_shape);
    visitor.on_attribute("broadcast_axes", m_broadcast_axes);
    visitor.on_attribute("initial_broadcast_axes", m_initial_broadcast_axes);
    return true;
}

shared_ptr<Node> op::v0::BroadcastLike::clone_with_new_inputs(const OutputVector& new_args) const
{
    if (new_args.size() != 2)
    {
        throw ngraph_error("Incorrect number of new arguments");
    }
    return make_shared<v0::BroadcastLike>(new_args.at(0), new_args.at(1), m_initial_broadcast_axes);
}

void op::v0::BroadcastLike::infer_shape()
{
    const Shape& in_shape = get_input_shape(0);
    m_shape = get_input_shape(1);
    m_broadcast_axes = m_initial_broadcast_axes;
    if (m_broadcast_axes.size() == 0)
    {
        for (size_t i = 0; i < m_shape.size(); ++i)
        {
            if (i < in_shape.size())
            {
                if (in_shape.at(i) == 1 && m_shape.at(i) > 1)
                {
                    m_broadcast_axes.insert(i);
                }
            }
            else
            {
                m_broadcast_axes.insert(i);
            }
        }
    }
}
