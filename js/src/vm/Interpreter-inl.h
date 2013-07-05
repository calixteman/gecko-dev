/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Interpreter_inl_h
#define vm_Interpreter_inl_h

#include "jsapi.h"
#include "jsbool.h"
#include "jscompartment.h"
#include "jsinfer.h"
#include "jslibmath.h"
#include "jsnum.h"
#include "jsstr.h"
#include "ion/Ion.h"
#include "ion/IonCompartment.h"
#include "vm/ForkJoin.h"
#include "vm/Interpreter.h"

#include "jsatominlines.h"
#include "jsfuninlines.h"
#include "jsinferinlines.h"
#include "jsopcodeinlines.h"
#include "vm/GlobalObject-inl.h"
#include "vm/Stack-inl.h"

namespace js {

/*
 * Compute the implicit |this| parameter for a call expression where the callee
 * funval was resolved from an unqualified name reference to a property on obj
 * (an object on the scope chain).
 *
 * We can avoid computing |this| eagerly and push the implicit callee-coerced
 * |this| value, undefined, if any of these conditions hold:
 *
 * 1. The nominal |this|, obj, is a global object.
 *
 * 2. The nominal |this|, obj, has one of Block, Call, or DeclEnv class (this
 *    is what IsCacheableNonGlobalScope tests). Such objects-as-scopes must be
 *    censored with undefined.
 *
 * Otherwise, we bind |this| to obj->thisObject(). Only names inside |with|
 * statements and embedding-specific scope objects fall into this category.
 *
 * If the callee is a strict mode function, then code implementing JSOP_THIS
 * in the interpreter and JITs will leave undefined as |this|. If funval is a
 * function not in strict mode, JSOP_THIS code replaces undefined with funval's
 * global.
 *
 * We set *vp to undefined early to reduce code size and bias this code for the
 * common and future-friendly cases.
 */
inline bool
ComputeImplicitThis(JSContext *cx, HandleObject obj, MutableHandleValue vp)
{
    vp.setUndefined();

    if (obj->is<GlobalObject>())
        return true;

    if (IsCacheableNonGlobalScope(obj))
        return true;

    JSObject *nobj = JSObject::thisObject(cx, obj);
    if (!nobj)
        return false;

    vp.setObject(*nobj);
    return true;
}

inline bool
ComputeThis(JSContext *cx, AbstractFramePtr frame)
{
    JS_ASSERT_IF(frame.isStackFrame(), !frame.asStackFrame()->runningInJit());
    if (frame.thisValue().isObject())
        return true;
    RootedValue thisv(cx, frame.thisValue());
    if (frame.isFunctionFrame()) {
        if (frame.fun()->strict() || frame.fun()->isSelfHostedBuiltin())
            return true;
        /*
         * Eval function frames have their own |this| slot, which is a copy of the function's
         * |this| slot. If we lazily wrap a primitive |this| in an eval function frame, the
         * eval's frame will get the wrapper, but the function's frame will not. To prevent
         * this, we always wrap a function's |this| before pushing an eval frame, and should
         * thus never see an unwrapped primitive in a non-strict eval function frame. Null
         * and undefined |this| values will unwrap to the same object in the function and
         * eval frames, so are not required to be wrapped.
         */
        JS_ASSERT_IF(frame.isEvalFrame(), thisv.isUndefined() || thisv.isNull());
    }
    bool modified;
    if (!BoxNonStrictThis(cx, &thisv, &modified))
        return false;

    frame.thisValue() = thisv;
    return true;
}

/*
 * Every possible consumer of MagicValue(JS_OPTIMIZED_ARGUMENTS) (as determined
 * by ScriptAnalysis::needsArgsObj) must check for these magic values and, when
 * one is received, act as if the value were the function's ArgumentsObject.
 * Additionally, it is possible that, after 'arguments' was copied into a
 * temporary, the arguments object has been created a some other failed guard
 * that called JSScript::argumentsOptimizationFailed. In this case, it is
 * always valid (and necessary) to replace JS_OPTIMIZED_ARGUMENTS with the real
 * arguments object.
 */
static inline bool
IsOptimizedArguments(AbstractFramePtr frame, Value *vp)
{
    if (vp->isMagic(JS_OPTIMIZED_ARGUMENTS) && frame.script()->needsArgsObj())
        *vp = ObjectValue(frame.argsObj());
    return vp->isMagic(JS_OPTIMIZED_ARGUMENTS);
}

/*
 * One optimized consumer of MagicValue(JS_OPTIMIZED_ARGUMENTS) is f.apply.
 * However, this speculation must be guarded before calling 'apply' in case it
 * is not the builtin Function.prototype.apply.
 */
static inline bool
GuardFunApplyArgumentsOptimization(JSContext *cx, AbstractFramePtr frame, HandleValue callee,
                                   Value *args, uint32_t argc)
{
    if (argc == 2 && IsOptimizedArguments(frame, &args[1])) {
        if (!IsNativeFunction(callee, js_fun_apply)) {
            RootedScript script(cx, frame.script());
            if (!JSScript::argumentsOptimizationFailed(cx, script))
                return false;
            args[1] = ObjectValue(frame.argsObj());
        }
    }

    return true;
}

/*
 * Return an object on which we should look for the properties of |value|.
 * This helps us implement the custom [[Get]] method that ES5's GetValue
 * algorithm uses for primitive values, without actually constructing the
 * temporary object that the specification does.
 *
 * For objects, return the object itself. For string, boolean, and number
 * primitive values, return the appropriate constructor's prototype. For
 * undefined and null, throw an error and return NULL, attributing the
 * problem to the value at |spindex| on the stack.
 */
JS_ALWAYS_INLINE JSObject *
ValuePropertyBearer(JSContext *cx, StackFrame *fp, HandleValue v, int spindex)
{
    if (v.isObject())
        return &v.toObject();

    GlobalObject &global = fp->global();

    if (v.isString())
        return global.getOrCreateStringPrototype(cx);
    if (v.isNumber())
        return global.getOrCreateNumberPrototype(cx);
    if (v.isBoolean())
        return global.getOrCreateBooleanPrototype(cx);

    JS_ASSERT(v.isNull() || v.isUndefined());
    js_ReportIsNullOrUndefined(cx, spindex, v, NullPtr());
    return NULL;
}

inline bool
NativeGet(JSContext *cx, JSObject *objArg, JSObject *pobjArg, Shape *shapeArg,
          unsigned getHow, MutableHandleValue vp)
{
    if (shapeArg->isDataDescriptor() && shapeArg->hasDefaultGetter()) {
        /* Fast path for Object instance properties. */
        JS_ASSERT(shapeArg->hasSlot());
        vp.set(pobjArg->nativeGetSlot(shapeArg->slot()));
    } else {
        RootedObject obj(cx, objArg);
        RootedObject pobj(cx, pobjArg);
        RootedShape shape(cx, shapeArg);
        if (!js_NativeGet(cx, obj, pobj, shape, getHow, vp))
            return false;
    }
    return true;
}

inline bool
GetLengthProperty(const Value &lval, MutableHandleValue vp)
{
    /* Optimize length accesses on strings, arrays, and arguments. */
    if (lval.isString()) {
        vp.setInt32(lval.toString()->length());
        return true;
    }
    if (lval.isObject()) {
        JSObject *obj = &lval.toObject();
        if (obj->is<ArrayObject>()) {
            vp.setNumber(obj->as<ArrayObject>().length());
            return true;
        }

        if (obj->is<ArgumentsObject>()) {
            ArgumentsObject *argsobj = &obj->as<ArgumentsObject>();
            if (!argsobj->hasOverriddenLength()) {
                uint32_t length = argsobj->initialLength();
                JS_ASSERT(length < INT32_MAX);
                vp.setInt32(int32_t(length));
                return true;
            }
        }

        if (obj->is<TypedArrayObject>()) {
            vp.setInt32(obj->as<TypedArrayObject>().length());
            return true;
        }
    }

    return false;
}

template <bool TypeOf> inline bool
FetchName(JSContext *cx, HandleObject obj, HandleObject obj2, HandlePropertyName name,
          HandleShape shape, MutableHandleValue vp)
{
    if (!shape) {
        if (TypeOf) {
            vp.setUndefined();
            return true;
        }
        JSAutoByteString printable;
        if (js_AtomToPrintableString(cx, name, &printable))
            js_ReportIsNotDefined(cx, printable.ptr());
        return false;
    }

    /* Take the slow path if shape was not found in a native object. */
    if (!obj->isNative() || !obj2->isNative()) {
        Rooted<jsid> id(cx, NameToId(name));
        if (!JSObject::getGeneric(cx, obj, obj, id, vp))
            return false;
    } else {
        Rooted<JSObject*> normalized(cx, obj);
        if (normalized->getClass() == &WithObject::class_ && !shape->hasDefaultGetter())
            normalized = &normalized->as<WithObject>().object();
        if (!NativeGet(cx, normalized, obj2, shape, 0, vp))
            return false;
    }
    return true;
}

inline bool
FetchNameNoGC(JSObject *pobj, Shape *shape, MutableHandleValue vp)
{
    if (!shape || !pobj->isNative() || !shape->isDataDescriptor() || !shape->hasDefaultGetter())
        return false;

    vp.set(pobj->nativeGetSlot(shape->slot()));
    return true;
}

inline bool
GetIntrinsicOperation(JSContext *cx, jsbytecode *pc, MutableHandleValue vp)
{
    RootedPropertyName name(cx, cx->currentScript()->getName(pc));
    return cx->global()->getIntrinsicValue(cx, name, vp);
}

inline bool
SetIntrinsicOperation(JSContext *cx, JSScript *script, jsbytecode *pc, HandleValue val)
{
    RootedPropertyName name(cx, script->getName(pc));
    return cx->global()->setIntrinsicValue(cx, name, val);
}

inline bool
SetNameOperation(JSContext *cx, JSScript *script, jsbytecode *pc, HandleObject scope,
                 HandleValue val)
{
    JS_ASSERT(*pc == JSOP_SETNAME || *pc == JSOP_SETGNAME);
    JS_ASSERT_IF(*pc == JSOP_SETGNAME, scope == cx->global());

    bool strict = script->strict;
    RootedPropertyName name(cx, script->getName(pc));
    RootedValue valCopy(cx, val);

    /*
     * In strict-mode, we need to trigger an error when trying to assign to an
     * undeclared global variable. To do this, we call SetPropertyHelper
     * directly and pass DNP_UNQUALIFIED.
     */
    if (scope->is<GlobalObject>()) {
        JS_ASSERT(!scope->getOps()->setProperty);
        RootedId id(cx, NameToId(name));
        return baseops::SetPropertyHelper(cx, scope, scope, id, DNP_UNQUALIFIED, &valCopy, strict);
    }

    return JSObject::setProperty(cx, scope, scope, name, &valCopy, strict);
}

inline bool
DefVarOrConstOperation(JSContext *cx, HandleObject varobj, HandlePropertyName dn, unsigned attrs)
{
    JS_ASSERT(varobj->isVarObj());

    RootedShape prop(cx);
    RootedObject obj2(cx);
    if (!JSObject::lookupProperty(cx, varobj, dn, &obj2, &prop))
        return false;

    /* Steps 8c, 8d. */
    if (!prop || (obj2 != varobj && varobj->is<GlobalObject>())) {
        RootedValue value(cx, UndefinedValue());
        if (!JSObject::defineProperty(cx, varobj, dn, value, JS_PropertyStub,
                                      JS_StrictPropertyStub, attrs)) {
            return false;
        }
    } else {
        /*
         * Extension: ordinarily we'd be done here -- but for |const|.  If we
         * see a redeclaration that's |const|, we consider it a conflict.
         */
        unsigned oldAttrs;
        if (!JSObject::getPropertyAttributes(cx, varobj, dn, &oldAttrs))
            return false;
        if (attrs & JSPROP_READONLY) {
            JSAutoByteString bytes;
            if (js_AtomToPrintableString(cx, dn, &bytes)) {
                JS_ALWAYS_FALSE(JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR,
                                                             js_GetErrorMessage,
                                                             NULL, JSMSG_REDECLARED_VAR,
                                                             (oldAttrs & JSPROP_READONLY)
                                                             ? "const"
                                                             : "var",
                                                             bytes.ptr()));
            }
            return false;
        }
    }

    return true;
}

inline bool
SetConstOperation(JSContext *cx, HandleObject varobj, HandlePropertyName name, HandleValue rval)
{
    return JSObject::defineProperty(cx, varobj, name, rval,
                                    JS_PropertyStub, JS_StrictPropertyStub,
                                    JSPROP_ENUMERATE | JSPROP_PERMANENT | JSPROP_READONLY);
}

inline void
InterpreterFrames::enableInterruptsIfRunning(JSScript *script)
{
    if (regs->fp()->script() == script)
        enabler.enable();
}

static JS_ALWAYS_INLINE bool
AddOperation(JSContext *cx, HandleScript script, jsbytecode *pc,
             MutableHandleValue lhs, MutableHandleValue rhs, Value *res)
{
    if (lhs.isInt32() && rhs.isInt32()) {
        int32_t l = lhs.toInt32(), r = rhs.toInt32();
        int32_t sum = l + r;
        if (JS_UNLIKELY(bool((l ^ sum) & (r ^ sum) & 0x80000000))) {
            res->setDouble(double(l) + double(r));
            types::TypeScript::MonitorOverflow(cx, script, pc);
        } else {
            res->setInt32(sum);
        }
        return true;
    }

    /*
     * If either operand is an object, any non-integer result must be
     * reported to inference.
     */
    bool lIsObject = lhs.isObject(), rIsObject = rhs.isObject();

    if (!ToPrimitive(cx, lhs))
        return false;
    if (!ToPrimitive(cx, rhs))
        return false;
    bool lIsString, rIsString;
    if ((lIsString = lhs.isString()) | (rIsString = rhs.isString())) {
        JSString *lstr, *rstr;
        if (lIsString) {
            lstr = lhs.toString();
        } else {
            lstr = ToString<CanGC>(cx, lhs);
            if (!lstr)
                return false;
        }
        if (rIsString) {
            rstr = rhs.toString();
        } else {
            // Save/restore lstr in case of GC activity under ToString.
            lhs.setString(lstr);
            rstr = ToString<CanGC>(cx, rhs);
            if (!rstr)
                return false;
            lstr = lhs.toString();
        }
        JSString *str = ConcatStrings<NoGC>(cx, lstr, rstr);
        if (!str) {
            RootedString nlstr(cx, lstr), nrstr(cx, rstr);
            str = ConcatStrings<CanGC>(cx, nlstr, nrstr);
            if (!str)
                return false;
        }
        if (lIsObject || rIsObject)
            types::TypeScript::MonitorString(cx, script, pc);
        res->setString(str);
    } else {
        double l, r;
        if (!ToNumber(cx, lhs, &l) || !ToNumber(cx, rhs, &r))
            return false;
        l += r;
        Value nres = NumberValue(l);
        if (nres.isDouble() &&
            (lIsObject || rIsObject || (!lhs.isDouble() && !rhs.isDouble()))) {
            types::TypeScript::MonitorOverflow(cx, script, pc);
        }
        *res = nres;
    }

    return true;
}

static JS_ALWAYS_INLINE bool
SubOperation(JSContext *cx, HandleScript script, jsbytecode *pc, HandleValue lhs, HandleValue rhs,
             Value *res)
{
    double d1, d2;
    if (!ToNumber(cx, lhs, &d1) || !ToNumber(cx, rhs, &d2))
        return false;
    double d = d1 - d2;
    if (!res->setNumber(d) && !(lhs.isDouble() || rhs.isDouble()))
        types::TypeScript::MonitorOverflow(cx, script, pc);
    return true;
}

static JS_ALWAYS_INLINE bool
MulOperation(JSContext *cx, HandleScript script, jsbytecode *pc, HandleValue lhs, HandleValue rhs,
             Value *res)
{
    double d1, d2;
    if (!ToNumber(cx, lhs, &d1) || !ToNumber(cx, rhs, &d2))
        return false;
    double d = d1 * d2;
    if (!res->setNumber(d) && !(lhs.isDouble() || rhs.isDouble()))
        types::TypeScript::MonitorOverflow(cx, script, pc);
    return true;
}

static JS_ALWAYS_INLINE bool
DivOperation(JSContext *cx, HandleScript script, jsbytecode *pc, HandleValue lhs, HandleValue rhs,
             Value *res)
{
    double d1, d2;
    if (!ToNumber(cx, lhs, &d1) || !ToNumber(cx, rhs, &d2))
        return false;
    res->setNumber(NumberDiv(d1, d2));

    if (d2 == 0 || (res->isDouble() && !(lhs.isDouble() || rhs.isDouble())))
        types::TypeScript::MonitorOverflow(cx, script, pc);
    return true;
}

static JS_ALWAYS_INLINE bool
ModOperation(JSContext *cx, HandleScript script, jsbytecode *pc, HandleValue lhs, HandleValue rhs,
             Value *res)
{
    int32_t l, r;
    if (lhs.isInt32() && rhs.isInt32() &&
        (l = lhs.toInt32()) >= 0 && (r = rhs.toInt32()) > 0) {
        int32_t mod = l % r;
        res->setInt32(mod);
        return true;
    }

    double d1, d2;
    if (!ToNumber(cx, lhs, &d1) || !ToNumber(cx, rhs, &d2))
        return false;

    res->setNumber(NumberMod(d1, d2));
    types::TypeScript::MonitorOverflow(cx, script, pc);
    return true;
}

static JS_ALWAYS_INLINE bool
NegOperation(JSContext *cx, HandleScript script, jsbytecode *pc, HandleValue val,
             MutableHandleValue res)
{
    /*
     * When the operand is int jsval, INT32_FITS_IN_JSVAL(i) implies
     * INT32_FITS_IN_JSVAL(-i) unless i is 0 or INT32_MIN when the
     * results, -0.0 or INT32_MAX + 1, are double values.
     */
    int32_t i;
    if (val.isInt32() && (i = val.toInt32()) != 0 && i != INT32_MIN) {
        i = -i;
        res.setInt32(i);
    } else {
        double d;
        if (!ToNumber(cx, val, &d))
            return false;
        d = -d;
        if (!res.setNumber(d) && !val.isDouble())
            types::TypeScript::MonitorOverflow(cx, script, pc);
    }

    return true;
}

static JS_ALWAYS_INLINE bool
ToIdOperation(JSContext *cx, HandleScript script, jsbytecode *pc, HandleValue objval,
              HandleValue idval, MutableHandleValue res)
{
    if (idval.isInt32()) {
        res.set(idval);
        return true;
    }

    JSObject *obj = ToObjectFromStack(cx, objval);
    if (!obj)
        return false;

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, idval, &id))
        return false;

    res.set(IdToValue(id));
    if (!res.isInt32())
        types::TypeScript::MonitorUnknown(cx, script, pc);
    return true;
}

static JS_ALWAYS_INLINE bool
GetObjectElementOperation(JSContext *cx, JSOp op, JSObject *objArg, bool wasObject,
                          HandleValue rref, MutableHandleValue res)
{
    do {
        // Don't call GetPcScript (needed for analysis) from inside Ion since it's expensive.
        bool analyze = cx->currentlyRunningInInterpreter();

        uint32_t index;
        if (IsDefinitelyIndex(rref, &index)) {
            if (analyze && !objArg->isNative() && !objArg->is<TypedArrayObject>()) {
                JSScript *script = NULL;
                jsbytecode *pc = NULL;
                types::TypeScript::GetPcScript(cx, &script, &pc);

                if (script->hasAnalysis())
                    script->analysis()->getCode(pc).nonNativeGetElement = true;
            }

            if (JSObject::getElementNoGC(cx, objArg, objArg, index, res.address()))
                break;

            RootedObject obj(cx, objArg);
            if (!JSObject::getElement(cx, obj, obj, index, res))
                return false;
            objArg = obj;
            break;
        }

        if (analyze) {
            JSScript *script = NULL;
            jsbytecode *pc = NULL;
            types::TypeScript::GetPcScript(cx, &script, &pc);

            if (script->hasAnalysis()) {
                script->analysis()->getCode(pc).getStringElement = true;

                if (!objArg->is<ArrayObject>() && !objArg->isNative() &&
                    !objArg->is<TypedArrayObject>())
                {
                    script->analysis()->getCode(pc).nonNativeGetElement = true;
                }
            }
        }

        if (ValueMightBeSpecial(rref)) {
            RootedObject obj(cx, objArg);
            Rooted<SpecialId> special(cx);
            res.set(rref);
            if (ValueIsSpecial(obj, res, &special, cx)) {
                if (!JSObject::getSpecial(cx, obj, obj, special, res))
                    return false;
                objArg = obj;
                break;
            }
            objArg = obj;
        }

        JSAtom *name = ToAtom<NoGC>(cx, rref);
        if (name) {
            if (name->isIndex(&index)) {
                if (JSObject::getElementNoGC(cx, objArg, objArg, index, res.address()))
                    break;
            } else {
                if (JSObject::getPropertyNoGC(cx, objArg, objArg, name->asPropertyName(), res.address()))
                    break;
            }
        }

        RootedObject obj(cx, objArg);

        name = ToAtom<CanGC>(cx, rref);
        if (!name)
            return false;

        if (name->isIndex(&index)) {
            if (!JSObject::getElement(cx, obj, obj, index, res))
                return false;
        } else {
            if (!JSObject::getProperty(cx, obj, obj, name->asPropertyName(), res))
                return false;
        }

        objArg = obj;
    } while (0);

#if JS_HAS_NO_SUCH_METHOD
    if (op == JSOP_CALLELEM && JS_UNLIKELY(res.isPrimitive()) && wasObject) {
        RootedObject obj(cx, objArg);
        if (!OnUnknownMethod(cx, obj, rref, res))
            return false;
    }
#endif

    assertSameCompartmentDebugOnly(cx, res);
    return true;
}

static JS_ALWAYS_INLINE bool
GetElemOptimizedArguments(JSContext *cx, AbstractFramePtr frame, MutableHandleValue lref,
                          HandleValue rref, MutableHandleValue res, bool *done)
{
    JS_ASSERT(!*done);

    if (IsOptimizedArguments(frame, lref.address())) {
        if (rref.isInt32()) {
            int32_t i = rref.toInt32();
            if (i >= 0 && uint32_t(i) < frame.numActualArgs()) {
                res.set(frame.unaliasedActual(i));
                *done = true;
                return true;
            }
        }

        RootedScript script(cx, frame.script());
        if (!JSScript::argumentsOptimizationFailed(cx, script))
            return false;

        lref.set(ObjectValue(frame.argsObj()));
    }

    return true;
}

static JS_ALWAYS_INLINE bool
GetElementOperation(JSContext *cx, JSOp op, MutableHandleValue lref, HandleValue rref,
                    MutableHandleValue res)
{
    JS_ASSERT(op == JSOP_GETELEM || op == JSOP_CALLELEM);

    uint32_t index;
    if (lref.isString() && IsDefinitelyIndex(rref, &index)) {
        JSString *str = lref.toString();
        if (index < str->length()) {
            str = cx->runtime()->staticStrings.getUnitStringForElement(cx, str, index);
            if (!str)
                return false;
            res.setString(str);
            return true;
        }
    }

    bool isObject = lref.isObject();
    JSObject *obj = ToObjectFromStack(cx, lref);
    if (!obj)
        return false;
    return GetObjectElementOperation(cx, op, obj, isObject, rref, res);
}

static JS_ALWAYS_INLINE bool
SetObjectElementOperation(JSContext *cx, Handle<JSObject*> obj, HandleId id, const Value &value,
                          bool strict, JSScript *maybeScript = NULL, jsbytecode *pc = NULL)
{
    RootedScript script(cx, maybeScript);
    types::TypeScript::MonitorAssign(cx, obj, id);

    if (obj->isNative() && JSID_IS_INT(id)) {
        uint32_t length = obj->getDenseInitializedLength();
        int32_t i = JSID_TO_INT(id);
        if ((uint32_t)i >= length) {
            // In an Ion activation, GetPcScript won't work.  For non-baseline activations,
            // that's ok, because optimized ion doesn't generate analysis info.  However,
            // baseline must generate this information, so it passes the script and pc in
            // as arguments.
            if (script || cx->currentlyRunningInInterpreter()) {
                JS_ASSERT(!!script == !!pc);
                if (!script)
                    types::TypeScript::GetPcScript(cx, script.address(), &pc);

                if (script->hasAnalysis())
                    script->analysis()->getCode(pc).arrayWriteHole = true;
            }
        }
    }

    if (obj->isNative() && !obj->setHadElementsAccess(cx))
        return false;

    RootedValue tmp(cx, value);
    return JSObject::setGeneric(cx, obj, obj, id, &tmp, strict);
}

static JS_ALWAYS_INLINE JSString *
TypeOfOperation(JSContext *cx, HandleValue v)
{
    JSType type = JS_TypeOfValue(cx, v);
    return TypeName(type, cx);
}

static JS_ALWAYS_INLINE bool
InitElemOperation(JSContext *cx, HandleObject obj, HandleValue idval, HandleValue val)
{
    JS_ASSERT(!val.isMagic(JS_ELEMENTS_HOLE));

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, idval, &id))
        return false;

    return JSObject::defineGeneric(cx, obj, id, val, NULL, NULL, JSPROP_ENUMERATE);
}

static JS_ALWAYS_INLINE bool
InitArrayElemOperation(JSContext *cx, jsbytecode *pc, HandleObject obj, uint32_t index, HandleValue val)
{
    JSOp op = JSOp(*pc);
    JS_ASSERT(op == JSOP_INITELEM_ARRAY || op == JSOP_INITELEM_INC);

    JS_ASSERT(obj->is<ArrayObject>());

    /*
     * If val is a hole, do not call JSObject::defineElement. In this case,
     * if the current op is the last element initialiser, set the array length
     * to one greater than id.
     */
    if (val.isMagic(JS_ELEMENTS_HOLE)) {
        JSOp next = JSOp(*GetNextPc(pc));

        if ((op == JSOP_INITELEM_ARRAY && next == JSOP_ENDINIT) ||
            (op == JSOP_INITELEM_INC && next == JSOP_POP))
        {
            if (!SetLengthProperty(cx, obj, index + 1))
                return false;
        }
    } else {
        if (!JSObject::defineElement(cx, obj, index, val, NULL, NULL, JSPROP_ENUMERATE))
            return false;
    }

    if (op == JSOP_INITELEM_INC && index == INT32_MAX) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_SPREAD_TOO_LARGE);
        return false;
    }

    return true;
}

#define RELATIONAL_OP(OP)                                                     \
    JS_BEGIN_MACRO                                                            \
        /* Optimize for two int-tagged operands (typical loop control). */    \
        if (lhs.isInt32() && rhs.isInt32()) {                                 \
            *res = lhs.toInt32() OP rhs.toInt32();                            \
        } else {                                                              \
            if (!ToPrimitive(cx, JSTYPE_NUMBER, lhs))                         \
                return false;                                                 \
            if (!ToPrimitive(cx, JSTYPE_NUMBER, rhs))                         \
                return false;                                                 \
            if (lhs.isString() && rhs.isString()) {                           \
                JSString *l = lhs.toString(), *r = rhs.toString();            \
                int32_t result;                                               \
                if (!CompareStrings(cx, l, r, &result))                       \
                    return false;                                             \
                *res = result OP 0;                                           \
            } else {                                                          \
                double l, r;                                                  \
                if (!ToNumber(cx, lhs, &l) || !ToNumber(cx, rhs, &r))         \
                    return false;;                                            \
                *res = (l OP r);                                              \
            }                                                                 \
        }                                                                     \
        return true;                                                          \
    JS_END_MACRO

static JS_ALWAYS_INLINE bool
LessThanOperation(JSContext *cx, MutableHandleValue lhs, MutableHandleValue rhs, bool *res) {
    RELATIONAL_OP(<);
}

static JS_ALWAYS_INLINE bool
LessThanOrEqualOperation(JSContext *cx, MutableHandleValue lhs, MutableHandleValue rhs, bool *res) {
    RELATIONAL_OP(<=);
}

static JS_ALWAYS_INLINE bool
GreaterThanOperation(JSContext *cx, MutableHandleValue lhs, MutableHandleValue rhs, bool *res) {
    RELATIONAL_OP(>);
}

static JS_ALWAYS_INLINE bool
GreaterThanOrEqualOperation(JSContext *cx, MutableHandleValue lhs, MutableHandleValue rhs, bool *res) {
    RELATIONAL_OP(>=);
}

static JS_ALWAYS_INLINE bool
BitNot(JSContext *cx, HandleValue in, int *out)
{
    int i;
    if (!ToInt32(cx, in, &i))
        return false;
    *out = ~i;
    return true;
}

static JS_ALWAYS_INLINE bool
BitXor(JSContext *cx, HandleValue lhs, HandleValue rhs, int *out)
{
    int left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left ^ right;
    return true;
}

static JS_ALWAYS_INLINE bool
BitOr(JSContext *cx, HandleValue lhs, HandleValue rhs, int *out)
{
    int left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left | right;
    return true;
}

static JS_ALWAYS_INLINE bool
BitAnd(JSContext *cx, HandleValue lhs, HandleValue rhs, int *out)
{
    int left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left & right;
    return true;
}

static JS_ALWAYS_INLINE bool
BitLsh(JSContext *cx, HandleValue lhs, HandleValue rhs, int *out)
{
    int32_t left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left << (right & 31);
    return true;
}

static JS_ALWAYS_INLINE bool
BitRsh(JSContext *cx, HandleValue lhs, HandleValue rhs, int *out)
{
    int32_t left, right;
    if (!ToInt32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    *out = left >> (right & 31);
    return true;
}

static JS_ALWAYS_INLINE bool
UrshOperation(JSContext *cx, HandleScript script, jsbytecode *pc,
              HandleValue lhs, HandleValue rhs, Value *out)
{
    uint32_t left;
    int32_t  right;
    if (!ToUint32(cx, lhs, &left) || !ToInt32(cx, rhs, &right))
        return false;
    left >>= right & 31;
    if (!out->setNumber(uint32_t(left)))
        types::TypeScript::MonitorOverflow(cx, script, pc);
    return true;
}

#undef RELATIONAL_OP

inline JSFunction *
ReportIfNotFunction(JSContext *cx, const Value &v, MaybeConstruct construct = NO_CONSTRUCT)
{
    if (v.isObject() && v.toObject().is<JSFunction>())
        return &v.toObject().as<JSFunction>();

    ReportIsNotFunction(cx, v, -1, construct);
    return NULL;
}

/*
 * FastInvokeGuard is used to optimize calls to JS functions from natives written
 * in C++, for instance Array.map. If the callee is not Ion-compiled, this will
 * just call Invoke. If the callee has a valid IonScript, however, it will enter
 * Ion directly.
 */
class FastInvokeGuard
{
    InvokeArgs args_;
    RootedFunction fun_;
    RootedScript script_;
#ifdef JS_ION
    // Constructing an IonContext is pretty expensive due to the TLS access,
    // so only do this if we have to.
    mozilla::Maybe<ion::IonContext> ictx_;
    bool useIon_;
#endif

  public:
    FastInvokeGuard(JSContext *cx, const Value &fval)
      : args_(cx)
      , fun_(cx)
      , script_(cx)
#ifdef JS_ION
      , useIon_(ion::IsEnabled(cx))
#endif
    {
        JS_ASSERT(!InParallelSection());
        initFunction(fval);
    }

    void initFunction(const Value &fval) {
        if (fval.isObject() && fval.toObject().is<JSFunction>()) {
            JSFunction *fun = &fval.toObject().as<JSFunction>();
            if (fun->isInterpreted())
                fun_ = fun;
        }
    }

    InvokeArgs &args() {
        return args_;
    }

    bool invoke(JSContext *cx) {
#ifdef JS_ION
        if (useIon_ && fun_) {
            if (!script_) {
                script_ = fun_->getOrCreateScript(cx);
                if (!script_)
                    return false;
            }
            if (ictx_.empty())
                ictx_.construct(cx, (js::ion::TempAllocator *)NULL);
            JS_ASSERT(fun_->nonLazyScript() == script_);

            ion::MethodStatus status = ion::CanEnterUsingFastInvoke(cx, script_, args_.length());
            if (status == ion::Method_Error)
                return false;
            if (status == ion::Method_Compiled) {
                ion::IonExecStatus result = ion::FastInvoke(cx, fun_, args_);
                if (IsErrorStatus(result))
                    return false;

                JS_ASSERT(result == ion::IonExec_Ok);
                return true;
            }

            JS_ASSERT(status == ion::Method_Skipped);

            if (script_->canIonCompile()) {
                // This script is not yet hot. Since calling into Ion is much
                // faster here, bump the use count a bit to account for this.
                script_->incUseCount(5);
            }
        }
#endif

        return Invoke(cx, args_);
    }

  private:
    FastInvokeGuard(const FastInvokeGuard& other) MOZ_DELETE;
    const FastInvokeGuard& operator=(const FastInvokeGuard& other) MOZ_DELETE;
};

}  /* namespace js */

#endif /* vm_Interpreter_inl_h */