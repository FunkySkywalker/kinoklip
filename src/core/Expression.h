// SPDX-License-Identifier: GPL-3.0-or-later
// Expression — a small, safe formula evaluator for the Expression modifier and for expressions
// typed into value fields (spec 119). No scripting engine: a formula is numbers, the variables
// time, value, n1, n2, n3, pi and e, references to other parameters, operators and a fixed list of
// maths functions. It can read values but never change anything, always ends (no loops, no
// recursion), and a failed formula reports an error instead of a value.
//
// The idea (a restricted, side-effect free expression language compiled once and evaluated many
// times) follows Blender's "simple expression" driver evaluator and Natron's parameter
// expressions; this code is written for Kinoklip.
#pragma once

#include <QString>
#include <QStringList>

#include <memory>

namespace kk::expr {

/// A compiled formula. Immutable and thread-safe: compile() hands out shared programs from a
/// cache, evaluation uses no shared state.
class Program
{
public:
    ~Program();

    /// Compiles @p text (cached by text). Returns nullptr and fills @p error (translated) when
    /// the formula is empty, too long (over 2000 characters), too deep, or has a syntax error.
    [[nodiscard]] static std::shared_ptr<const Program> compile(
        const QString& text, QString* error = nullptr);

    /// The parameter references in order of first use, as written ("Blur1.size", "size"): any
    /// identifier that is not a variable, constant or function. The caller supplies their values
    /// to evaluate() in this order.
    [[nodiscard]] const QStringList& references() const { return m_refs; }

    /// Inputs of one evaluation.
    struct Inputs
    {
        double time = 0.0; ///< "time": the composition frame
        double value = 0.0; ///< "value": what the parameter would be without the Expression
        double n[3] = {0.0, 0.0, 0.0}; ///< "n1" .. "n3"
        const double* refs = nullptr; ///< values of references(), or nullptr (all 0)
    };
    /// The result; 0 when the maths has no finite result (division by zero, sqrt(-1), ...).
    [[nodiscard]] double evaluate(const Inputs& in) const;

    /// Names of the functions the language has (for the tooltip and the docs).
    [[nodiscard]] static QStringList functionNames();

    struct Node; ///< implementation detail

private:
    Program() = default;
    std::unique_ptr<Node> m_root;
    QStringList m_refs;
    friend class Parser;
};

} // namespace kk::expr
