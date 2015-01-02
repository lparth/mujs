#include "jsi.h"
#include "jscompile.h"
#include "jsvalue.h"
#include "jsrun.h"

#include "utf.h"

static void jsR_run(js_State *J, js_Function *F);

/* Push values on stack */

#define STACK (J->stack)
#define TOP (J->top)
#define BOT (J->bot)

static void js_stackoverflow(js_State *J)
{
	STACK[TOP].type = JS_TLITSTR;
	STACK[TOP].u.litstr = "stack overflow";
	++TOP;
	js_throw(J);
}

static void js_outofmemory(js_State *J)
{
	STACK[TOP].type = JS_TLITSTR;
	STACK[TOP].u.litstr = "out of memory";
	++TOP;
	js_throw(J);
}

void *js_malloc(js_State *J, unsigned int size)
{
	void *ptr = J->alloc(J->actx, NULL, size);
	if (!ptr)
		js_outofmemory(J);
	return ptr;
}

void *js_realloc(js_State *J, void *ptr, unsigned int size)
{
	ptr = J->alloc(J->actx, ptr, size);
	if (!ptr)
		js_outofmemory(J);
	return ptr;
}

void js_free(js_State *J, void *ptr)
{
	J->alloc(J->actx, ptr, 0);
}

js_String *jsV_newmemstring(js_State *J, const char *s, int n)
{
	js_String *v = js_malloc(J, offsetof(js_String, p) + n + 1);
	memcpy(v->p, s, n);
	v->p[n] = 0;
	v->gcmark = 0;
	v->gcnext = J->gcstr;
	J->gcstr = v;
	++J->gccounter;
	return v;
}

#define CHECKSTACK(n) if (TOP + n >= JS_STACKSIZE) js_stackoverflow(J)

void js_pushvalue(js_State *J, js_Value v)
{
	CHECKSTACK(1);
	STACK[TOP] = v;
	++TOP;
}

void js_pushundefined(js_State *J)
{
	CHECKSTACK(1);
	STACK[TOP].type = JS_TUNDEFINED;
	++TOP;
}

void js_pushnull(js_State *J)
{
	CHECKSTACK(1);
	STACK[TOP].type = JS_TNULL;
	++TOP;
}

void js_pushboolean(js_State *J, int v)
{
	CHECKSTACK(1);
	STACK[TOP].type = JS_TBOOLEAN;
	STACK[TOP].u.boolean = !!v;
	++TOP;
}

void js_pushnumber(js_State *J, double v)
{
	CHECKSTACK(1);
	STACK[TOP].type = JS_TNUMBER;
	STACK[TOP].u.number = v;
	++TOP;
}

void js_pushstring(js_State *J, const char *v)
{
	int n = strlen(v);
	CHECKSTACK(1);
	if (n < 16) {
		STACK[TOP].type = JS_TSHRSTR;
		memcpy(STACK[TOP].u.shrstr, v, n);
		STACK[TOP].u.shrstr[n] = 0;
	} else {
		STACK[TOP].type = JS_TMEMSTR;
		STACK[TOP].u.memstr = jsV_newmemstring(J, v, n);
	}
	++TOP;
}

void js_pushlstring(js_State *J, const char *v, unsigned int n)
{
	CHECKSTACK(1);
	if (n < 16) {
		STACK[TOP].type = JS_TSHRSTR;
		memcpy(STACK[TOP].u.shrstr, v, n);
		STACK[TOP].u.shrstr[n] = 0;
	} else {
		STACK[TOP].type = JS_TMEMSTR;
		STACK[TOP].u.memstr = jsV_newmemstring(J, v, n);
	}
	++TOP;
}

void js_pushliteral(js_State *J, const char *v)
{
	CHECKSTACK(1);
	STACK[TOP].type = JS_TLITSTR;
	STACK[TOP].u.litstr = v;
	++TOP;
}

void js_pushobject(js_State *J, js_Object *v)
{
	CHECKSTACK(1);
	STACK[TOP].type = JS_TOBJECT;
	STACK[TOP].u.object = v;
	++TOP;
}

void js_pushglobal(js_State *J)
{
	js_pushobject(J, J->G);
}

void js_currentfunction(js_State *J)
{
	CHECKSTACK(1);
	STACK[TOP] = STACK[BOT-1];
	++TOP;
}

/* Read values from stack */

static js_Value *stackidx(js_State *J, int idx)
{
	static js_Value undefined = { {0}, {0}, JS_TUNDEFINED };
	idx = idx < 0 ? TOP + idx : BOT + idx;
	if (idx < 0 || idx >= TOP)
		return &undefined;
	return STACK + idx;
}

js_Value *js_tovalue(js_State *J, int idx)
{
	return stackidx(J, idx);
}

int js_isdefined(js_State *J, int idx) { return stackidx(J, idx)->type != JS_TUNDEFINED; }
int js_isundefined(js_State *J, int idx) { return stackidx(J, idx)->type == JS_TUNDEFINED; }
int js_isnull(js_State *J, int idx) { return stackidx(J, idx)->type == JS_TNULL; }
int js_isboolean(js_State *J, int idx) { return stackidx(J, idx)->type == JS_TBOOLEAN; }
int js_isnumber(js_State *J, int idx) { return stackidx(J, idx)->type == JS_TNUMBER; }
int js_isstring(js_State *J, int idx) { enum js_Type t = stackidx(J, idx)->type; return t == JS_TSHRSTR || t == JS_TLITSTR || t == JS_TMEMSTR; }
int js_isprimitive(js_State *J, int idx) { return stackidx(J, idx)->type != JS_TOBJECT; }
int js_isobject(js_State *J, int idx) { return stackidx(J, idx)->type == JS_TOBJECT; }

int js_iscallable(js_State *J, int idx)
{
	js_Value *v = stackidx(J, idx);
	if (v->type == JS_TOBJECT)
		return v->u.object->type == JS_CFUNCTION ||
			v->u.object->type == JS_CSCRIPT ||
			v->u.object->type == JS_CCFUNCTION;
	return 0;
}

int js_isarray(js_State *J, int idx)
{
	js_Value *v = stackidx(J, idx);
	return v->type == JS_TOBJECT && v->u.object->type == JS_CARRAY;
}

int js_isregexp(js_State *J, int idx)
{
	js_Value *v = stackidx(J, idx);
	return v->type == JS_TOBJECT && v->u.object->type == JS_CREGEXP;
}

static int js_isiterator(js_State *J, int idx)
{
	js_Value *v = stackidx(J, idx);
	return v->type == JS_TOBJECT && v->u.object->type == JS_CITERATOR;
}

int js_isuserdata(js_State *J, int idx, const char *tag)
{
	js_Value *v = stackidx(J, idx);
	if (v->type == JS_TOBJECT && v->u.object->type == JS_CUSERDATA)
		return !strcmp(tag, v->u.object->u.user.tag);
	return 0;
}

static const char *js_typeof(js_State *J, int idx)
{
	js_Value *v = stackidx(J, idx);
	switch (v->type) {
	default:
	case JS_TSHRSTR: return "string";
	case JS_TUNDEFINED: return "undefined";
	case JS_TNULL: return "object";
	case JS_TBOOLEAN: return "boolean";
	case JS_TNUMBER: return "number";
	case JS_TLITSTR: return "string";
	case JS_TMEMSTR: return "string";
	case JS_TOBJECT:
		if (v->u.object->type == JS_CFUNCTION || v->u.object->type == JS_CCFUNCTION)
			return "function";
		return "object";
	}
}

int js_toboolean(js_State *J, int idx)
{
	return jsV_toboolean(J, stackidx(J, idx));
}

double js_tonumber(js_State *J, int idx)
{
	return jsV_tonumber(J, stackidx(J, idx));
}

double js_tointeger(js_State *J, int idx)
{
	return jsV_numbertointeger(jsV_tonumber(J, stackidx(J, idx)));
}

int js_toint32(js_State *J, int idx)
{
	return jsV_numbertoint32(jsV_tonumber(J, stackidx(J, idx)));
}

unsigned int js_touint32(js_State *J, int idx)
{
	return jsV_numbertouint32(jsV_tonumber(J, stackidx(J, idx)));
}

short js_toint16(js_State *J, int idx)
{
	return jsV_numbertoint16(jsV_tonumber(J, stackidx(J, idx)));
}

unsigned short js_touint16(js_State *J, int idx)
{
	return jsV_numbertouint16(jsV_tonumber(J, stackidx(J, idx)));
}

const char *js_tostring(js_State *J, int idx)
{
	return jsV_tostring(J, stackidx(J, idx));
}

js_Object *js_toobject(js_State *J, int idx)
{
	return jsV_toobject(J, stackidx(J, idx));
}

void js_toprimitive(js_State *J, int idx, int hint)
{
	jsV_toprimitive(J, stackidx(J, idx), hint);
}

js_Regexp *js_toregexp(js_State *J, int idx)
{
	js_Value *v = stackidx(J, idx);
	if (v->type == JS_TOBJECT && v->u.object->type == JS_CREGEXP)
		return &v->u.object->u.r;
	js_typeerror(J, "not a regexp");
}

void *js_touserdata(js_State *J, int idx, const char *tag)
{
	js_Value *v = stackidx(J, idx);
	if (v->type == JS_TOBJECT && v->u.object->type == JS_CUSERDATA)
		if (!strcmp(tag, v->u.object->u.user.tag))
			return v->u.object->u.user.data;
	js_typeerror(J, "not a %s", tag);
}

static js_Object *jsR_tofunction(js_State *J, int idx)
{
	js_Value *v = stackidx(J, idx);
	if (v->type == JS_TUNDEFINED || v->type == JS_TNULL)
		return NULL;
	if (v->type == JS_TOBJECT)
		if (v->u.object->type == JS_CFUNCTION || v->u.object->type == JS_CCFUNCTION)
			return v->u.object;
	js_typeerror(J, "not a function");
}

/* Stack manipulation */

int js_gettop(js_State *J)
{
	return TOP - BOT;
}

void js_pop(js_State *J, int n)
{
	TOP -= n;
	if (TOP < BOT) {
		TOP = BOT;
		js_error(J, "stack underflow!");
	}
}

void js_remove(js_State *J, int idx)
{
	idx = idx < 0 ? TOP + idx : BOT + idx;
	if (idx < BOT || idx >= TOP)
		js_error(J, "stack error!");
	for (;idx < TOP - 1; ++idx)
		STACK[idx] = STACK[idx+1];
	--TOP;
}

void js_copy(js_State *J, int idx)
{
	CHECKSTACK(1);
	STACK[TOP] = *stackidx(J, idx);
	++TOP;
}

void js_dup(js_State *J)
{
	CHECKSTACK(1);
	STACK[TOP] = STACK[TOP-1];
	++TOP;
}

void js_dup2(js_State *J)
{
	CHECKSTACK(2);
	STACK[TOP] = STACK[TOP-2];
	STACK[TOP+1] = STACK[TOP-1];
	TOP += 2;
}

void js_rot2(js_State *J)
{
	/* A B -> B A */
	js_Value tmp = STACK[TOP-1];	/* A B (B) */
	STACK[TOP-1] = STACK[TOP-2];	/* A A */
	STACK[TOP-2] = tmp;		/* B A */
}

void js_rot3(js_State *J)
{
	/* A B C -> C A B */
	js_Value tmp = STACK[TOP-1];	/* A B C (C) */
	STACK[TOP-1] = STACK[TOP-2];	/* A B B */
	STACK[TOP-2] = STACK[TOP-3];	/* A A B */
	STACK[TOP-3] = tmp;		/* C A B */
}

void js_rot4(js_State *J)
{
	/* A B C D -> D A B C */
	js_Value tmp = STACK[TOP-1];	/* A B C D (D) */
	STACK[TOP-1] = STACK[TOP-2];	/* A B C C */
	STACK[TOP-2] = STACK[TOP-3];	/* A B B C */
	STACK[TOP-3] = STACK[TOP-4];	/* A A B C */
	STACK[TOP-4] = tmp;		/* D A B C */
}

void js_rot2pop1(js_State *J)
{
	/* A B -> B */
	STACK[TOP-2] = STACK[TOP-1];
	--TOP;
}

void js_rot3pop2(js_State *J)
{
	/* A B C -> C */
	STACK[TOP-3] = STACK[TOP-1];
	TOP -= 2;
}

void js_rot(js_State *J, int n)
{
	int i;
	js_Value tmp = STACK[TOP-1];
	for (i = 1; i < n; ++i)
		STACK[TOP-i] = STACK[TOP-i-1];
	STACK[TOP-i] = tmp;
}

/* Property access that takes care of attributes and getters/setters */

int js_isarrayindex(js_State *J, const char *str, unsigned int *idx)
{
	char buf[32];
	*idx = jsV_numbertouint32(jsV_stringtonumber(J, str));
	sprintf(buf, "%u", *idx);
	return !strcmp(buf, str);
}

static void js_pushrune(js_State *J, Rune rune)
{
	char buf[UTFmax + 1];
	if (rune > 0) {
		buf[runetochar(buf, &rune)] = 0;
		js_pushstring(J, buf);
	} else {
		js_pushundefined(J);
	}
}


static int jsR_hasproperty(js_State *J, js_Object *obj, const char *name)
{
	js_Property *ref;
	unsigned int k;

	if (obj->type == JS_CARRAY) {
		if (!strcmp(name, "length")) {
			js_pushnumber(J, obj->u.a.length);
			return 1;
		}
	}

	if (obj->type == JS_CSTRING) {
		if (!strcmp(name, "length")) {
			js_pushnumber(J, obj->u.s.length);
			return 1;
		}
		if (js_isarrayindex(J, name, &k)) {
			js_pushrune(J, js_runeat(J, obj->u.s.string, k));
			return 1;
		}
	}

	if (obj->type == JS_CREGEXP) {
		if (!strcmp(name, "source")) {
			js_pushliteral(J, obj->u.r.source);
			return 1;
		}
		if (!strcmp(name, "global")) {
			js_pushboolean(J, obj->u.r.flags & JS_REGEXP_G);
			return 1;
		}
		if (!strcmp(name, "ignoreCase")) {
			js_pushboolean(J, obj->u.r.flags & JS_REGEXP_I);
			return 1;
		}
		if (!strcmp(name, "multiline")) {
			js_pushboolean(J, obj->u.r.flags & JS_REGEXP_M);
			return 1;
		}
		if (!strcmp(name, "lastIndex")) {
			js_pushnumber(J, obj->u.r.last);
			return 1;
		}
	}

	ref = jsV_getproperty(J, obj, name);
	if (ref) {
		if (ref->getter) {
			js_pushobject(J, ref->getter);
			js_pushobject(J, obj);
			js_call(J, 0);
		} else {
			js_pushvalue(J, ref->value);
		}
		return 1;
	}

	return 0;
}

static void jsR_getproperty(js_State *J, js_Object *obj, const char *name)
{
	if (!jsR_hasproperty(J, obj, name))
		js_pushundefined(J);
}

static void jsR_setproperty(js_State *J, js_Object *obj, const char *name, js_Value *value)
{
	js_Property *ref;
	unsigned int k;
	int own;

	if (obj->type == JS_CARRAY) {
		if (!strcmp(name, "length")) {
			double rawlen = jsV_tonumber(J, value);
			unsigned int newlen = jsV_numbertouint32(rawlen);
			if (newlen != rawlen)
				js_rangeerror(J, "array length");
			jsV_resizearray(J, obj, newlen);
			return;
		}
		if (js_isarrayindex(J, name, &k))
			if (k >= obj->u.a.length)
				obj->u.a.length = k + 1;
	}

	if (obj->type == JS_CSTRING) {
		if (!strcmp(name, "length"))
			return;
		if (js_isarrayindex(J, name, &k))
			if (js_runeat(J, obj->u.s.string, k))
				return;
	}

	if (obj->type == JS_CREGEXP) {
		if (!strcmp(name, "source")) return;
		if (!strcmp(name, "global")) return;
		if (!strcmp(name, "ignoreCase")) return;
		if (!strcmp(name, "multiline")) return;
		if (!strcmp(name, "lastIndex")) {
			obj->u.r.last = jsV_tointeger(J, value);
			return;
		}
	}

	/* First try to find a setter in prototype chain */
	ref = jsV_getpropertyx(J, obj, name, &own);
	if (ref && ref->setter) {
		js_pushobject(J, ref->setter);
		js_pushobject(J, obj);
		js_pushvalue(J, *value);
		js_call(J, 1);
		js_pop(J, 1);
		return;
	}

	/* Property not found on this object, so create one */
	if (!ref || !own)
		ref = jsV_setproperty(J, obj, name);

	if (ref && !(ref->atts & JS_READONLY))
		ref->value = *value;
}

static void jsR_defproperty(js_State *J, js_Object *obj, const char *name,
	int atts, js_Value *value, js_Object *getter, js_Object *setter)
{
	js_Property *ref;
	unsigned int k;

	if (obj->type == JS_CARRAY)
		if (!strcmp(name, "length"))
			return;

	if (obj->type == JS_CSTRING) {
		if (!strcmp(name, "length"))
			return;
		if (js_isarrayindex(J, name, &k))
			if (js_runeat(J, obj->u.s.string, k))
				return;
	}

	if (obj->type == JS_CREGEXP) {
		if (!strcmp(name, "source")) return;
		if (!strcmp(name, "global")) return;
		if (!strcmp(name, "ignoreCase")) return;
		if (!strcmp(name, "multiline")) return;
		if (!strcmp(name, "lastIndex")) return;
	}

	ref = jsV_setproperty(J, obj, name);
	if (ref) {
		if (value && !(ref->atts & JS_READONLY))
			ref->value = *value;
		if (getter && !(ref->atts & JS_DONTCONF))
			ref->getter = getter;
		if (setter && !(ref->atts & JS_DONTCONF))
			ref->setter = setter;
		ref->atts |= atts;
	}
}

static int jsR_delproperty(js_State *J, js_Object *obj, const char *name)
{
	js_Property *ref;
	unsigned int k;

	if (obj->type == JS_CARRAY)
		if (!strcmp(name, "length")) return 0;

	if (obj->type == JS_CSTRING) {
		if (!strcmp(name, "length"))
			return 0;
		if (js_isarrayindex(J, name, &k))
			if (js_runeat(J, obj->u.s.string, k))
				return 0;
	}

	if (obj->type == JS_CREGEXP) {
		if (!strcmp(name, "source")) return 0;
		if (!strcmp(name, "global")) return 0;
		if (!strcmp(name, "ignoreCase")) return 0;
		if (!strcmp(name, "multiline")) return 0;
		if (!strcmp(name, "lastIndex")) return 0;
	}

	ref = jsV_getownproperty(J, obj, name);
	if (ref) {
		if (ref->atts & JS_DONTCONF)
			return 0;
		jsV_delproperty(J, obj, name);
	}
	return 1;
}

/* Registry, global and object property accessors */

const char *js_ref(js_State *J)
{
	js_Value *v = stackidx(J, -1);
	const char *s;
	char buf[32];
	switch (v->type) {
	case JS_TUNDEFINED: s = "_Undefined"; break;
	case JS_TNULL: s = "_Null"; break;
	case JS_TBOOLEAN:
		s = v->u.boolean ? "_True" : "_False";
		break;
	case JS_TOBJECT:
		sprintf(buf, "%p", (void*)v->u.object);
		s = js_intern(J, buf);
		break;
	default:
		sprintf(buf, "%d", J->nextref++);
		s = js_intern(J, buf);
		break;
	}
	js_setregistry(J, s);
	return s;
}

void js_unref(js_State *J, const char *ref)
{
	js_delregistry(J, ref);
}

void js_getregistry(js_State *J, const char *name)
{
	jsR_getproperty(J, J->R, name);
}

void js_setregistry(js_State *J, const char *name)
{
	jsR_setproperty(J, J->R, name, stackidx(J, -1));
	js_pop(J, 1);
}

void js_delregistry(js_State *J, const char *name)
{
	jsR_delproperty(J, J->R, name);
}

void js_getglobal(js_State *J, const char *name)
{
	jsR_getproperty(J, J->G, name);
}

void js_setglobal(js_State *J, const char *name)
{
	jsR_setproperty(J, J->G, name, stackidx(J, -1));
	js_pop(J, 1);
}

void js_defglobal(js_State *J, const char *name, int atts)
{
	jsR_defproperty(J, J->G, name, atts, stackidx(J, -1), NULL, NULL);
	js_pop(J, 1);
}

void js_getproperty(js_State *J, int idx, const char *name)
{
	jsR_getproperty(J, js_toobject(J, idx), name);
}

void js_setproperty(js_State *J, int idx, const char *name)
{
	jsR_setproperty(J, js_toobject(J, idx), name, stackidx(J, -1));
	js_pop(J, 1);
}

void js_defproperty(js_State *J, int idx, const char *name, int atts)
{
	jsR_defproperty(J, js_toobject(J, idx), name, atts, stackidx(J, -1), NULL, NULL);
	js_pop(J, 1);
}

void js_delproperty(js_State *J, int idx, const char *name)
{
	jsR_delproperty(J, js_toobject(J, idx), name);
}

void js_defaccessor(js_State *J, int idx, const char *name, int atts)
{
	jsR_defproperty(J, js_toobject(J, idx), name, atts, NULL, jsR_tofunction(J, -2), jsR_tofunction(J, -1));
	js_pop(J, 2);
}

int js_hasproperty(js_State *J, int idx, const char *name)
{
	return jsR_hasproperty(J, js_toobject(J, idx), name);
}

/* Iterator */

void js_pushiterator(js_State *J, int idx, int own)
{
	js_pushobject(J, jsV_newiterator(J, js_toobject(J, idx), own));
}

const char *js_nextiterator(js_State *J, int idx)
{
	return jsV_nextiterator(J, js_toobject(J, idx));
}

/* Environment records */

js_Environment *jsR_newenvironment(js_State *J, js_Object *vars, js_Environment *outer)
{
	js_Environment *E = js_malloc(J, sizeof *E);
	E->gcmark = 0;
	E->gcnext = J->gcenv;
	J->gcenv = E;
	++J->gccounter;

	E->outer = outer;
	E->variables = vars;
	return E;
}

static void js_initvar(js_State *J, const char *name, int idx)
{
	jsR_defproperty(J, J->E->variables, name, JS_DONTENUM | JS_DONTCONF, stackidx(J, idx), NULL, NULL);
}

static void js_defvar(js_State *J, const char *name)
{
	jsR_defproperty(J, J->E->variables, name, JS_DONTENUM | JS_DONTCONF, NULL, NULL, NULL);
}

static int js_hasvar(js_State *J, const char *name)
{
	js_Environment *E = J->E;
	do {
		js_Property *ref = jsV_getproperty(J, E->variables, name);
		if (ref) {
			if (ref->getter) {
				js_pushobject(J, ref->getter);
				js_pushobject(J, E->variables);
				js_call(J, 0);
			} else {
				js_pushvalue(J, ref->value);
			}
			return 1;
		}
		E = E->outer;
	} while (E);
	return 0;
}

static void js_setvar(js_State *J, const char *name)
{
	js_Environment *E = J->E;
	do {
		js_Property *ref = jsV_getproperty(J, E->variables, name);
		if (ref) {
			if (ref->setter) {
				js_pushobject(J, ref->setter);
				js_pushobject(J, E->variables);
				js_copy(J, -3);
				js_call(J, 1);
				js_pop(J, 1);
				return;
			}
			if (!(ref->atts & JS_READONLY))
				ref->value = *stackidx(J, -1);
			return;
		}
		E = E->outer;
	} while (E);
	jsR_setproperty(J, J->G, name, stackidx(J, -1));
}

static int js_delvar(js_State *J, const char *name)
{
	js_Environment *E = J->E;
	do {
		js_Property *ref = jsV_getownproperty(J, E->variables, name);
		if (ref) {
			if (ref->atts & JS_DONTCONF)
				return 0;
			jsV_delproperty(J, E->variables, name);
			return 1;
		}
		E = E->outer;
	} while (E);
	return jsR_delproperty(J, J->G, name);
}

/* Function calls */

static void jsR_savescope(js_State *J, js_Environment *newE)
{
	if (J->envtop + 1 >= JS_ENVLIMIT)
		js_stackoverflow(J);
	J->envstack[J->envtop++] = J->E;
	J->E = newE;
}

static void jsR_restorescope(js_State *J)
{
	J->E = J->envstack[--J->envtop];
}

static void jsR_calllwfunction(js_State *J, unsigned int n, js_Function *F, js_Environment *scope)
{
	js_Value v;
	unsigned int i;

	jsR_savescope(J, scope);

	if (n > F->numparams) {
		js_pop(J, F->numparams - n);
		n = F->numparams;
	}
	for (i = n; i < F->varlen; ++i)
		js_pushundefined(J);

	jsR_run(J, F);
	v = *stackidx(J, -1);
	TOP = --BOT; /* clear stack */
	js_pushvalue(J, v);

	jsR_restorescope(J);
}

static void jsR_callfunction(js_State *J, unsigned int n, js_Function *F, js_Environment *scope)
{
	js_Value v;
	unsigned int i;

	scope = jsR_newenvironment(J, jsV_newobject(J, JS_COBJECT, NULL), scope);

	jsR_savescope(J, scope);

	if (F->arguments) {
		js_newobject(J);
		js_currentfunction(J);
		js_defproperty(J, -2, "callee", JS_DONTENUM);
		js_pushnumber(J, n);
		js_defproperty(J, -2, "length", JS_DONTENUM);
		for (i = 0; i < n; ++i) {
			js_copy(J, i + 1);
			js_setindex(J, -2, i);
		}
		js_initvar(J, "arguments", -1);
		js_pop(J, 1);
	}

	for (i = 0; i < F->numparams; ++i) {
		if (i < n)
			js_initvar(J, F->vartab[i], i + 1);
		else {
			js_pushundefined(J);
			js_initvar(J, F->vartab[i], -1);
			js_pop(J, 1);
		}
	}
	js_pop(J, n);

	jsR_run(J, F);
	v = *stackidx(J, -1);
	TOP = --BOT; /* clear stack */
	js_pushvalue(J, v);

	jsR_restorescope(J);
}

static void jsR_callscript(js_State *J, unsigned int n, js_Function *F, js_Environment *scope)
{
	js_Value v;

	if (scope)
		jsR_savescope(J, scope);

	js_pop(J, n);
	jsR_run(J, F);
	v = *stackidx(J, -1);
	TOP = --BOT; /* clear stack */
	js_pushvalue(J, v);

	if (scope)
		jsR_restorescope(J);
}

static void jsR_callcfunction(js_State *J, unsigned int n, unsigned int min, js_CFunction F)
{
	unsigned int i;
	js_Value v;

	for (i = n; i < min; ++i)
		js_pushundefined(J);

	F(J);
	v = *stackidx(J, -1);
	TOP = --BOT; /* clear stack */
	js_pushvalue(J, v);
}

static void jsR_pushtrace(js_State *J, const char *name, const char *file, int line)
{
	if (++J->tracetop == JS_ENVLIMIT)
		js_error(J, "call stack overflow");
	J->trace[J->tracetop].name = name;
	J->trace[J->tracetop].file = file;
	J->trace[J->tracetop].line = line;
}

void js_call(js_State *J, int n)
{
	js_Object *obj;
	int savebot;

	if (!js_iscallable(J, -n-2))
		js_typeerror(J, "called object is not a function");

	obj = js_toobject(J, -n-2);

	savebot = BOT;
	BOT = TOP - n - 1;

	if (obj->type == JS_CFUNCTION) {
		jsR_pushtrace(J, obj->u.f.function->name, obj->u.f.function->filename, obj->u.f.function->line);
		if (obj->u.f.function->lightweight)
			jsR_calllwfunction(J, n, obj->u.f.function, obj->u.f.scope);
		else
			jsR_callfunction(J, n, obj->u.f.function, obj->u.f.scope);
		--J->tracetop;
	} else if (obj->type == JS_CSCRIPT) {
		jsR_pushtrace(J, obj->u.f.function->name, obj->u.f.function->filename, obj->u.f.function->line);
		jsR_callscript(J, n, obj->u.f.function, obj->u.f.scope);
		--J->tracetop;
	} else if (obj->type == JS_CCFUNCTION) {
		jsR_pushtrace(J, obj->u.c.name, "[C]", 0);
		jsR_callcfunction(J, n, obj->u.c.length, obj->u.c.function);
		--J->tracetop;
	}

	BOT = savebot;
}

void js_construct(js_State *J, int n)
{
	js_Object *obj;
	js_Object *prototype;
	js_Object *newobj;

	if (!js_iscallable(J, -n-1))
		js_typeerror(J, "called object is not a function");

	obj = js_toobject(J, -n-1);

	/* built-in constructors create their own objects, give them a 'null' this */
	if (obj->type == JS_CCFUNCTION && obj->u.c.constructor) {
		int savebot = BOT;
		js_pushnull(J);
		if (n > 0)
			js_rot(J, n + 1);
		BOT = TOP - n - 1;

		jsR_pushtrace(J, obj->u.c.name, "[C]", 0);
		jsR_callcfunction(J, n, obj->u.c.length, obj->u.c.constructor);
		--J->tracetop;

		BOT = savebot;
		return;
	}

	/* extract the function object's prototype property */
	js_getproperty(J, -n - 1, "prototype");
	if (js_isobject(J, -1))
		prototype = js_toobject(J, -1);
	else
		prototype = J->Object_prototype;
	js_pop(J, 1);

	/* create a new object with above prototype, and shift it into the 'this' slot */
	newobj = jsV_newobject(J, JS_COBJECT, prototype);
	js_pushobject(J, newobj);
	if (n > 0)
		js_rot(J, n + 1);

	/* call the function */
	js_call(J, n);

	/* if result is not an object, return the original object we created */
	if (!js_isobject(J, -1)) {
		js_pop(J, 1);
		js_pushobject(J, newobj);
	}
}

int js_pconstruct(js_State *J, int n)
{
	if (js_try(J))
		return 1;
	js_construct(J, n);
	js_endtry(J);
	return 0;
}

int js_pcall(js_State *J, int n)
{
	if (js_try(J))
		return 1;
	js_call(J, n);
	js_endtry(J);
	return 0;
}

/* Exceptions */

void js_savetry(js_State *J, js_Instruction *pc)
{
	if (J->trytop == JS_TRYLIMIT)
		js_error(J, "try: exception stack overflow");
	J->trybuf[J->trytop].E = J->E;
	J->trybuf[J->trytop].envtop = J->envtop;
	J->trybuf[J->trytop].tracetop = J->tracetop;
	J->trybuf[J->trytop].top = J->top;
	J->trybuf[J->trytop].bot = J->bot;
	J->trybuf[J->trytop].pc = pc;
}

void js_throw(js_State *J)
{
	if (J->trytop > 0) {
		js_Value v = *stackidx(J, -1);
		--J->trytop;
		J->E = J->trybuf[J->trytop].E;
		J->envtop = J->trybuf[J->trytop].envtop;
		J->tracetop = J->trybuf[J->trytop].tracetop;
		J->top = J->trybuf[J->trytop].top;
		J->bot = J->trybuf[J->trytop].bot;
		js_pushvalue(J, v);
		longjmp(J->trybuf[J->trytop].buf, 1);
	}
	if (J->panic)
		J->panic(J);
	abort();
}

/* Main interpreter loop */

static void jsR_dumpstack(js_State *J)
{
	int i;
	printf("stack {\n");
	for (i = 0; i < TOP; ++i) {
		putchar(i == BOT ? '>' : ' ');
		printf("% 4d: ", i);
		js_dumpvalue(J, STACK[i]);
		putchar('\n');
	}
	printf("}\n");
}

static void jsR_dumpenvironment(js_State *J, js_Environment *E, int d)
{
	printf("scope %d ", d);
	js_dumpobject(J, E->variables);
	if (E->outer)
		jsR_dumpenvironment(J, E->outer, d+1);
}

void js_stacktrace(js_State *J)
{
	int n;
	printf("stack trace:\n");
	for (n = J->tracetop; n >= 0; --n) {
		const char *name = J->trace[n].name;
		const char *file = J->trace[n].file;
		int line = J->trace[n].line;
		if (line > 0)
			printf("\t%s:%d: in function '%s'\n", file, line, name);
		else
			printf("\t%s: in function '%s'\n", file, name);
	}
}

void js_trap(js_State *J, int pc)
{
	if (pc > 0) {
		js_Function *F = STACK[BOT-1].u.object->u.f.function;
		printf("trap at %d in function ", pc);
		jsC_dumpfunction(J, F);
	}
	jsR_dumpstack(J);
	jsR_dumpenvironment(J, J->E, 0);
	js_stacktrace(J);
}

static void jsR_run(js_State *J, js_Function *F)
{
	js_Function **FT = F->funtab;
	double *NT = F->numtab;
	const char **ST = F->strtab;
	js_Instruction *pcstart = F->code;
	js_Instruction *pc = F->code;
	enum js_OpCode opcode;
	int offset;

	const char *str;
	char buf[32];
	js_Object *obj;
	double x, y;
	unsigned int ux, uy;
	int ix, iy, okay;
	int b;

	while (1) {
		if (J->gccounter > JS_GCLIMIT) {
			J->gccounter = 0;
			js_gc(J, 0);
		}

		opcode = *pc++;
		switch (opcode) {
		case OP_POP: js_pop(J, 1); break;
		case OP_DUP: js_dup(J); break;
		case OP_DUP2: js_dup2(J); break;
		case OP_ROT2: js_rot2(J); break;
		case OP_ROT3: js_rot3(J); break;
		case OP_ROT4: js_rot4(J); break;

		case OP_NUMBER_0: js_pushnumber(J, 0); break;
		case OP_NUMBER_1: js_pushnumber(J, 1); break;
		case OP_NUMBER_POS: js_pushnumber(J, *pc++); break;
		case OP_NUMBER_NEG: js_pushnumber(J, -(*pc++)); break;
		case OP_NUMBER: js_pushnumber(J, NT[*pc++]); break;
		case OP_STRING: js_pushliteral(J, ST[*pc++]); break;

		case OP_CLOSURE: js_newfunction(J, FT[*pc++], J->E); break;
		case OP_NEWOBJECT: js_newobject(J); break;
		case OP_NEWARRAY: js_newarray(J); break;
		case OP_NEWREGEXP: js_newregexp(J, ST[pc[0]], pc[1]); pc += 2; break;

		case OP_UNDEF: js_pushundefined(J); break;
		case OP_NULL: js_pushnull(J); break;
		case OP_TRUE: js_pushboolean(J, 1); break;
		case OP_FALSE: js_pushboolean(J, 0); break;

		case OP_THIS: js_copy(J, 0); break;
		case OP_GLOBAL: js_pushobject(J, J->G); break;
		case OP_CURRENT: js_currentfunction(J); break;

		case OP_INITLOCAL:
			STACK[BOT + *pc++] = STACK[--TOP];
			break;

		case OP_GETLOCAL:
			CHECKSTACK(1);
			STACK[TOP++] = STACK[BOT + *pc++];
			break;

		case OP_SETLOCAL:
			STACK[BOT + *pc++] = STACK[TOP-1];
			break;

		case OP_DELLOCAL:
			++pc;
			js_pushboolean(J, 0);
			break;

		case OP_INITVAR:
			js_initvar(J, ST[*pc++], -1);
			js_pop(J, 1);
			break;

		case OP_DEFVAR:
			js_defvar(J, ST[*pc++]);
			break;

		case OP_GETVAR:
			str = ST[*pc++];
			if (!js_hasvar(J, str))
				js_referenceerror(J, "%s is not defined", str);
			break;

		case OP_HASVAR:
			if (!js_hasvar(J, ST[*pc++]))
				js_pushundefined(J);
			break;

		case OP_SETVAR:
			js_setvar(J, ST[*pc++]);
			break;

		case OP_DELVAR:
			b = js_delvar(J, ST[*pc++]);
			js_pushboolean(J, b);
			break;

		case OP_IN:
			str = js_tostring(J, -2);
			if (!js_isobject(J, -1))
				js_typeerror(J, "operand to 'in' is not an object");
			b = js_hasproperty(J, -1, str);
			js_pop(J, 2 + b);
			js_pushboolean(J, b);
			break;

		case OP_INITGETTER:
			obj = js_toobject(J, -3);
			str = js_tostring(J, -2);
			jsR_defproperty(J, obj, str, 0, NULL, jsR_tofunction(J, -1), NULL);
			js_pop(J, 2);
			break;

		case OP_INITSETTER:
			obj = js_toobject(J, -3);
			str = js_tostring(J, -2);
			jsR_defproperty(J, obj, str, 0, NULL, NULL, jsR_tofunction(J, -1));
			js_pop(J, 2);
			break;

		case OP_INITPROP:
			str = js_tostring(J, -2);
			obj = js_toobject(J, -3);
			jsR_setproperty(J, obj, str, stackidx(J, -1));
			js_pop(J, 2);
			break;

		case OP_INITPROP_S:
			str = ST[*pc++];
			obj = js_toobject(J, -2);
			jsR_setproperty(J, obj, str, stackidx(J, -1));
			js_pop(J, 1);
			break;

		case OP_INITPROP_N:
			str = jsV_numbertostring(J, buf, *pc++);
			obj = js_toobject(J, -2);
			jsR_setproperty(J, obj, str, stackidx(J, -1));
			js_pop(J, 1);
			break;

		case OP_GETPROP:
			str = js_tostring(J, -1);
			obj = js_toobject(J, -2);
			jsR_getproperty(J, obj, str);
			js_rot3pop2(J);
			break;

		case OP_GETPROP_S:
			str = ST[*pc++];
			obj = js_toobject(J, -1);
			jsR_getproperty(J, obj, str);
			js_rot2pop1(J);
			break;

		case OP_SETPROP:
			str = js_tostring(J, -2);
			obj = js_toobject(J, -3);
			jsR_setproperty(J, obj, str, stackidx(J, -1));
			js_rot3pop2(J);
			break;

		case OP_SETPROP_S:
			str = ST[*pc++];
			obj = js_toobject(J, -2);
			jsR_setproperty(J, obj, str, stackidx(J, -1));
			js_rot2pop1(J);
			break;

		case OP_DELPROP:
			str = js_tostring(J, -1);
			obj = js_toobject(J, -2);
			b = jsR_delproperty(J, obj, str);
			js_pop(J, 2);
			js_pushboolean(J, b);
			break;

		case OP_DELPROP_S:
			str = ST[*pc++];
			obj = js_toobject(J, -1);
			b = jsR_delproperty(J, obj, str);
			js_pop(J, 1);
			js_pushboolean(J, b);
			break;

		case OP_ITERATOR:
			if (!js_isundefined(J, -1) && !js_isnull(J, -1)) {
				obj = jsV_newiterator(J, js_toobject(J, -1), 0);
				js_pop(J, 1);
				js_pushobject(J, obj);
			}
			break;

		case OP_NEXTITER:
			if (js_isiterator(J, -1)) {
				obj = js_toobject(J, -1);
				str = jsV_nextiterator(J, obj);
				if (str) {
					js_pushliteral(J, str);
					js_pushboolean(J, 1);
				} else {
					js_pop(J, 1);
					js_pushboolean(J, 0);
				}
			} else {
				js_pop(J, 1);
				js_pushboolean(J, 0);
			}
			break;

		/* Function calls */

		case OP_CALL:
			js_call(J, *pc++);
			break;

		case OP_NEW:
			js_construct(J, *pc++);
			break;

		/* Unary operators */

		case OP_TYPEOF:
			str = js_typeof(J, -1);
			js_pop(J, 1);
			js_pushliteral(J, str);
			break;

		case OP_POS:
			x = js_tonumber(J, -1);
			js_pop(J, 1);
			js_pushnumber(J, x);
			break;

		case OP_NEG:
			x = js_tonumber(J, -1);
			js_pop(J, 1);
			js_pushnumber(J, -x);
			break;

		case OP_BITNOT:
			ix = js_toint32(J, -1);
			js_pop(J, 1);
			js_pushnumber(J, ~ix);
			break;

		case OP_LOGNOT:
			b = js_toboolean(J, -1);
			js_pop(J, 1);
			js_pushboolean(J, !b);
			break;

		case OP_INC:
			x = js_tonumber(J, -1);
			js_pop(J, 1);
			js_pushnumber(J, x + 1);
			break;

		case OP_DEC:
			x = js_tonumber(J, -1);
			js_pop(J, 1);
			js_pushnumber(J, x - 1);
			break;

		case OP_POSTINC:
			x = js_tonumber(J, -1);
			js_pop(J, 1);
			js_pushnumber(J, x + 1);
			js_pushnumber(J, x);
			break;

		case OP_POSTDEC:
			x = js_tonumber(J, -1);
			js_pop(J, 1);
			js_pushnumber(J, x - 1);
			js_pushnumber(J, x);
			break;

		/* Multiplicative operators */

		case OP_MUL:
			x = js_tonumber(J, -2);
			y = js_tonumber(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, x * y);
			break;

		case OP_DIV:
			x = js_tonumber(J, -2);
			y = js_tonumber(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, x / y);
			break;

		case OP_MOD:
			x = js_tonumber(J, -2);
			y = js_tonumber(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, fmod(x, y));
			break;

		/* Additive operators */

		case OP_ADD:
			js_concat(J);
			break;

		case OP_SUB:
			x = js_tonumber(J, -2);
			y = js_tonumber(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, x - y);
			break;

		/* Shift operators */

		case OP_SHL:
			ix = js_toint32(J, -2);
			uy = js_touint32(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, ix << (uy & 0x1F));
			break;

		case OP_SHR:
			ix = js_toint32(J, -2);
			uy = js_touint32(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, ix >> (uy & 0x1F));
			break;

		case OP_USHR:
			ux = js_touint32(J, -2);
			uy = js_touint32(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, ux >> (uy & 0x1F));
			break;

		/* Relational operators */

		case OP_LT: b = js_compare(J, &okay); js_pop(J, 2); js_pushboolean(J, okay && b < 0); break;
		case OP_GT: b = js_compare(J, &okay); js_pop(J, 2); js_pushboolean(J, okay && b > 0); break;
		case OP_LE: b = js_compare(J, &okay); js_pop(J, 2); js_pushboolean(J, okay && b <= 0); break;
		case OP_GE: b = js_compare(J, &okay); js_pop(J, 2); js_pushboolean(J, okay && b >= 0); break;

		case OP_INSTANCEOF:
			b = js_instanceof(J);
			js_pop(J, 2);
			js_pushboolean(J, b);
			break;

		/* Equality */

		case OP_EQ: b = js_equal(J); js_pop(J, 2); js_pushboolean(J, b); break;
		case OP_NE: b = js_equal(J); js_pop(J, 2); js_pushboolean(J, !b); break;
		case OP_STRICTEQ: b = js_strictequal(J); js_pop(J, 2); js_pushboolean(J, b); break;
		case OP_STRICTNE: b = js_strictequal(J); js_pop(J, 2); js_pushboolean(J, !b); break;

		case OP_JCASE:
			offset = *pc++;
			b = js_strictequal(J);
			if (b) {
				js_pop(J, 2);
				pc = pcstart + offset;
			} else {
				js_pop(J, 1);
			}
			break;

		/* Binary bitwise operators */

		case OP_BITAND:
			ix = js_toint32(J, -2);
			iy = js_toint32(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, ix & iy);
			break;

		case OP_BITXOR:
			ix = js_toint32(J, -2);
			iy = js_toint32(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, ix ^ iy);
			break;

		case OP_BITOR:
			ix = js_toint32(J, -2);
			iy = js_toint32(J, -1);
			js_pop(J, 2);
			js_pushnumber(J, ix | iy);
			break;

		/* Try and Catch */

		case OP_THROW:
			js_throw(J);

		case OP_TRY:
			offset = *pc++;
			if (js_trypc(J, pc)) {
				pc = J->trybuf[J->trytop].pc;
			} else {
				pc = pcstart + offset;
			}
			break;

		case OP_ENDTRY:
			js_endtry(J);
			break;

		case OP_CATCH:
			str = ST[*pc++];
			obj = jsV_newobject(J, JS_COBJECT, NULL);
			js_pushobject(J, obj);
			js_rot2(J);
			js_setproperty(J, -2, str);
			J->E = jsR_newenvironment(J, obj, J->E);
			js_pop(J, 1);
			break;

		case OP_ENDCATCH:
			J->E = J->E->outer;
			break;

		/* With */

		case OP_WITH:
			obj = js_toobject(J, -1);
			J->E = jsR_newenvironment(J, obj, J->E);
			js_pop(J, 1);
			break;

		case OP_ENDWITH:
			J->E = J->E->outer;
			break;

		/* Branching */

		case OP_DEBUGGER:
			js_trap(J, (int)(pc - pcstart) - 1);
			break;

		case OP_JUMP:
			pc = pcstart + *pc;
			break;

		case OP_JTRUE:
			offset = *pc++;
			b = js_toboolean(J, -1);
			js_pop(J, 1);
			if (b)
				pc = pcstart + offset;
			break;

		case OP_JFALSE:
			offset = *pc++;
			b = js_toboolean(J, -1);
			js_pop(J, 1);
			if (!b)
				pc = pcstart + offset;
			break;

		case OP_RETURN:
			return;

		case OP_LINE:
			J->trace[J->tracetop].line = *pc++;
			break;
		}
	}
}
