/*
 * tclProc.c --
 *
 *	This file contains routines that implement Tcl procedures,
 *	including the "proc" and "uplevel" commands.
 *
 * Copyright (c) 1987-1993 The Regents of the University of California.
 * Copyright (c) 1994-1998 Sun Microsystems, Inc.
 * Copyright (c) 2007 Daniel A. Steffen <das@users.sourceforge.net>
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"
#include "tclCompile.h"

/*
 * Prototypes for static functions in this file
 */

static void	ProcBodyDup _ANSI_ARGS_((Tcl_Obj *srcPtr, Tcl_Obj *dupPtr));
static void	ProcBodyFree _ANSI_ARGS_((Tcl_Obj *objPtr));
static int	ProcBodySetFromAny _ANSI_ARGS_((Tcl_Interp *interp,
		Tcl_Obj *objPtr));
static void	ProcBodyUpdateString _ANSI_ARGS_((Tcl_Obj *objPtr));
static int	ProcCompileProc _ANSI_ARGS_((Tcl_Interp *interp,
		    Proc *procPtr, Tcl_Obj *bodyPtr, Namespace *nsPtr,
		    CONST char *description, CONST char *procName,
		    Proc **procPtrPtr));
static  int	ProcessProcResultCode _ANSI_ARGS_((Tcl_Interp *interp,
		    char *procName, int nameLen, int returnCode));
static int	TclCompileNoOp _ANSI_ARGS_((Tcl_Interp *interp,
		    Tcl_Parse *parsePtr, struct CompileEnv *envPtr));

/*
 * The ProcBodyObjType type
 */

Tcl_ObjType tclProcBodyType = {
    "procbody",			/* name for this type */
    ProcBodyFree,		/* FreeInternalRep procedure */
    ProcBodyDup,		/* DupInternalRep procedure */
    ProcBodyUpdateString,	/* UpdateString procedure */
    ProcBodySetFromAny		/* SetFromAny procedure */
};

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ProcObjCmd --
 *
 *	This object-based procedure is invoked to process the "proc" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl object result value.
 *
 * Side effects:
 *	A new procedure gets created.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_ProcObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    register Interp *iPtr = (Interp *) interp;
    Proc *procPtr;
    char *fullName;
    CONST char *procName, *procArgs, *procBody;
    Namespace *nsPtr, *altNsPtr, *cxtNsPtr;
    Tcl_Command cmd;
    Tcl_DString ds;

    if (objc != 4) {
	Tcl_WrongNumArgs(interp, 1, objv, "name args body");
	return TCL_ERROR;
    }

    /*
     * Determine the namespace where the procedure should reside. Unless
     * the command name includes namespace qualifiers, this will be the
     * current namespace.
     */

    fullName = TclGetString(objv[1]);
    TclGetNamespaceForQualName(interp, fullName, (Namespace *) NULL,
	    0, &nsPtr, &altNsPtr, &cxtNsPtr, &procName);

    if (nsPtr == NULL) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"can't create procedure \"", fullName,
		"\": unknown namespace", (char *) NULL);
        return TCL_ERROR;
    }
    if (procName == NULL) {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"can't create procedure \"", fullName,
		"\": bad procedure name", (char *) NULL);
        return TCL_ERROR;
    }
    if ((nsPtr != iPtr->globalNsPtr)
	    && (procName != NULL) && (procName[0] == ':')) {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"can't create procedure \"", procName,
		"\" in non-global namespace with name starting with \":\"",
	        (char *) NULL);
        return TCL_ERROR;
    }

    /*
     *  Create the data structure to represent the procedure.
     */
    if (TclCreateProc(interp, nsPtr, procName, objv[2], objv[3],
        &procPtr) != TCL_OK) {
        return TCL_ERROR;
    }

    /*
     * Now create a command for the procedure. This will initially be in
     * the current namespace unless the procedure's name included namespace
     * qualifiers. To create the new command in the right namespace, we
     * generate a fully qualified name for it.
     */

    Tcl_DStringInit(&ds);
    if (nsPtr != iPtr->globalNsPtr) {
	Tcl_DStringAppend(&ds, nsPtr->fullName, -1);
	Tcl_DStringAppend(&ds, "::", 2);
    }
    Tcl_DStringAppend(&ds, procName, -1);

    Tcl_CreateCommand(interp, Tcl_DStringValue(&ds), TclProcInterpProc,
	    (ClientData) procPtr, TclProcDeleteProc);
    cmd = Tcl_CreateObjCommand(interp, Tcl_DStringValue(&ds),
	    TclObjInterpProc, (ClientData) procPtr, TclProcDeleteProc);

    Tcl_DStringFree(&ds);
    /*
     * Now initialize the new procedure's cmdPtr field. This will be used
     * later when the procedure is called to determine what namespace the
     * procedure will run in. This will be different than the current
     * namespace if the proc was renamed into a different namespace.
     */

    procPtr->cmdPtr = (Command *) cmd;

#ifdef TCL_TIP280
    /* TIP #280 Remember the line the procedure body is starting on. In a
     * Byte code context we ask the engine to provide us with the necessary
     * information. This is for the initialization of the byte code compiler
     * when the body is used for the first time.
     */

    if (iPtr->cmdFramePtr) {
        CmdFrame context = *iPtr->cmdFramePtr;

	if (context.type == TCL_LOCATION_BC) {
	    TclGetSrcInfoForPc (&context);
	    /* May get path in context */
	} else if (context.type == TCL_LOCATION_SOURCE) {
	    /* context now holds another reference */
	    Tcl_IncrRefCount (context.data.eval.path);
	}

	/* type == TCL_LOCATION_PREBC implies that 'line' is NULL here!  We
	 * cannot assume that 'line' is valid here, we have to check. If the
	 * outer context is an eval (bc, prebc, eval) we do not save any
	 * information. Counting relative to the beginning of the proc body is
	 * more sensible than counting relative to the outer eval block.
	 */

	if ((context.type == TCL_LOCATION_SOURCE) &&
	    context.line &&
	    (context.nline >= 4) &&
	    (context.line [3] >= 0)) {
	    int       isNew;
	    Tcl_HashEntry* hePtr;
	    CmdFrame* cfPtr = (CmdFrame*) ckalloc (sizeof (CmdFrame));

	    cfPtr->level    = -1;
	    cfPtr->type     = context.type;
	    cfPtr->line     = (int*) ckalloc (sizeof (int));
	    cfPtr->line [0] = context.line [3];
	    cfPtr->nline    = 1;
	    cfPtr->framePtr = NULL;
	    cfPtr->nextPtr  = NULL;

	    if (context.type == TCL_LOCATION_SOURCE) {
	        cfPtr->data.eval.path = context.data.eval.path;
		/* Transfer of reference. The reference going away (release of
		 * the context) is replaced by the reference in the
		 * constructed cmdframe */
	    } else {
	        cfPtr->type = TCL_LOCATION_EVAL;
		cfPtr->data.eval.path = NULL;
	    }

	    cfPtr->cmd.str.cmd = NULL;
	    cfPtr->cmd.str.len = 0;

	    hePtr = Tcl_CreateHashEntry (iPtr->linePBodyPtr, (char*) procPtr,
					 &isNew);
	    if (!isNew) {
		/*
		 * Get the old command frame and release it.  See also
		 * TclProcCleanupProc in this file. Currently it seems as if
		 * only the procbodytest::proc command of the testsuite is
		 * able to trigger this situation.
		 */

		CmdFrame* cfOldPtr = (CmdFrame *) Tcl_GetHashValue(hePtr);

		if (cfOldPtr->type == TCL_LOCATION_SOURCE) {
		    Tcl_DecrRefCount(cfOldPtr->data.eval.path);
		    cfOldPtr->data.eval.path = NULL;
		}
		ckfree((char *) cfOldPtr->line);
		cfOldPtr->line = NULL;
		ckfree((char *) cfOldPtr);
	    }
	    Tcl_SetHashValue (hePtr, cfPtr);
	}
    }
#endif

    /*
     * Optimize for noop procs: if the body is not precompiled (like a TclPro
     * procbody), and the argument list is just "args" and the body is empty,
     * define a compileProc to compile a noop.
     *
     * Notes:
     *   - cannot be done for any argument list without having different
     *     compiled/not-compiled behaviour in the "wrong argument #" case,
     *     or making this code much more complicated. In any case, it doesn't
     *     seem to make a lot of sense to verify the number of arguments we
     *     are about to ignore ...
     *   - could be enhanced to handle also non-empty bodies that contain
     *     only comments; however, parsing the body will slow down the
     *     compilation of all procs whose argument list is just _args_ */

    if (objv[3]->typePtr == &tclProcBodyType) {
	goto done;
    }

    procArgs = Tcl_GetString(objv[2]);

    while (*procArgs == ' ') {
	procArgs++;
    }

    if ((procArgs[0] == 'a') && (strncmp(procArgs, "args", 4) == 0)) {
	procArgs +=4;
	while(*procArgs != '\0') {
	    if (*procArgs != ' ') {
		goto done;
	    }
	    procArgs++;
	}

	/*
	 * The argument list is just "args"; check the body
	 */

	procBody = Tcl_GetString(objv[3]);
	while (*procBody != '\0') {
	    if (!isspace(UCHAR(*procBody))) {
		goto done;
	    }
	    procBody++;
	}

	/*
	 * The body is just spaces: link the compileProc
	 */

	((Command *) cmd)->compileProc = TclCompileNoOp;
    }

 done:
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCreateProc --
 *
 *	Creates the data associated with a Tcl procedure definition.
 *	This procedure knows how to handle two types of body objects:
 *	strings and procbody. Strings are the traditional (and common) value
 *	for bodies, procbody are values created by extensions that have
 *	loaded a previously compiled script.
 *
 * Results:
 *	Returns TCL_OK on success, along with a pointer to a Tcl
 *	procedure definition in procPtrPtr.  This definition should
 *	be freed by calling TclCleanupProc() when it is no longer
 *	needed.  Returns TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	If anything goes wrong, this procedure returns an error
 *	message in the interpreter.
 *
 *----------------------------------------------------------------------
 */
int
TclCreateProc(interp, nsPtr, procName, argsPtr, bodyPtr, procPtrPtr)
    Tcl_Interp *interp;         /* interpreter containing proc */
    Namespace *nsPtr;           /* namespace containing this proc */
    CONST char *procName;       /* unqualified name of this proc */
    Tcl_Obj *argsPtr;           /* description of arguments */
    Tcl_Obj *bodyPtr;           /* command body */
    Proc **procPtrPtr;          /* returns:  pointer to proc data */
{
    Interp *iPtr = (Interp*)interp;
    CONST char **argArray = NULL;

    register Proc *procPtr;
    int i, length, result, numArgs;
    CONST char *args, *bytes, *p;
    register CompiledLocal *localPtr = NULL;
    Tcl_Obj *defPtr;
    int precompiled = 0;

    if (bodyPtr->typePtr == &tclProcBodyType) {
        /*
         * Because the body is a TclProProcBody, the actual body is already
         * compiled, and it is not shared with anyone else, so it's OK not to
         * unshare it (as a matter of fact, it is bad to unshare it, because
         * there may be no source code).
         *
         * We don't create and initialize a Proc structure for the procedure;
         * rather, we use what is in the body object. Note that
         * we initialize its cmdPtr field below after we've created the command
         * for the procedure. We increment the ref count of the Proc struct
         * since the command (soon to be created) will be holding a reference
         * to it.
         */

        procPtr = (Proc *) bodyPtr->internalRep.otherValuePtr;
        procPtr->iPtr = iPtr;
        procPtr->refCount++;
        precompiled = 1;
    } else {
        /*
         * If the procedure's body object is shared because its string value is
         * identical to, e.g., the body of another procedure, we must create a
         * private copy for this procedure to use. Such sharing of procedure
         * bodies is rare but can cause problems. A procedure body is compiled
         * in a context that includes the number of compiler-allocated "slots"
         * for local variables. Each formal parameter is given a local variable
         * slot (the "procPtr->numCompiledLocals = numArgs" assignment
         * below). This means that the same code can not be shared by two
         * procedures that have a different number of arguments, even if their
         * bodies are identical. Note that we don't use Tcl_DuplicateObj since
         * we would not want any bytecode internal representation.
         */

        if (Tcl_IsShared(bodyPtr)) {
#ifdef TCL_TIP280
	    Tcl_Obj* sharedBodyPtr = bodyPtr;
#endif
            bytes = Tcl_GetStringFromObj(bodyPtr, &length);
            bodyPtr = Tcl_NewStringObj(bytes, length);
#ifdef TCL_TIP280
	    /*
	     * TIP #280.
	     * Ensure that the continuation line data for the original body is
	     * not lost and applies to the new body as well.
	     */

	    TclContinuationsCopy (bodyPtr, sharedBodyPtr);
#endif
        }

        /*
         * Create and initialize a Proc structure for the procedure. Note that
         * we initialize its cmdPtr field below after we've created the command
         * for the procedure. We increment the ref count of the procedure's
         * body object since there will be a reference to it in the Proc
         * structure.
         */

        Tcl_IncrRefCount(bodyPtr);

        procPtr = (Proc *) ckalloc(sizeof(Proc));
        procPtr->iPtr = iPtr;
        procPtr->refCount = 1;
        procPtr->bodyPtr = bodyPtr;
        procPtr->numArgs  = 0;	/* actual argument count is set below. */
        procPtr->numCompiledLocals = 0;
        procPtr->firstLocalPtr = NULL;
        procPtr->lastLocalPtr = NULL;
    }

    /*
     * Break up the argument list into argument specifiers, then process
     * each argument specifier.
     * If the body is precompiled, processing is limited to checking that
     * the the parsed argument is consistent with the one stored in the
     * Proc.
     * THIS FAILS IF THE ARG LIST OBJECT'S STRING REP CONTAINS NULLS.
     */

    args = Tcl_GetStringFromObj(argsPtr, &length);
    result = Tcl_SplitList(interp, args, &numArgs, &argArray);
    if (result != TCL_OK) {
        goto procError;
    }

    if (precompiled) {
        if (numArgs > procPtr->numArgs) {
            char buf[64 + TCL_INTEGER_SPACE + TCL_INTEGER_SPACE];
            sprintf(buf, "\": arg list contains %d entries, precompiled header expects %d",
                    numArgs, procPtr->numArgs);
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                    "procedure \"", procName,
                    buf, (char *) NULL);
            goto procError;
        }
        localPtr = procPtr->firstLocalPtr;
    } else {
        procPtr->numArgs = numArgs;
        procPtr->numCompiledLocals = numArgs;
    }
    for (i = 0;  i < numArgs;  i++) {
        int fieldCount, nameLength, valueLength;
        CONST char **fieldValues;

        /*
         * Now divide the specifier up into name and default.
         */

        result = Tcl_SplitList(interp, argArray[i], &fieldCount,
                &fieldValues);
        if (result != TCL_OK) {
            goto procError;
        }
        if (fieldCount > 2) {
            ckfree((char *) fieldValues);
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                    "too many fields in argument specifier \"",
                    argArray[i], "\"", (char *) NULL);
            goto procError;
        }
        if ((fieldCount == 0) || (*fieldValues[0] == 0)) {
            ckfree((char *) fieldValues);
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                    "procedure \"", procName,
                    "\" has argument with no name", (char *) NULL);
            goto procError;
        }

        nameLength = strlen(fieldValues[0]);
        if (fieldCount == 2) {
            valueLength = strlen(fieldValues[1]);
        } else {
            valueLength = 0;
        }

        /*
         * Check that the formal parameter name is a scalar.
         */

        p = fieldValues[0];
        while (*p != '\0') {
            if (*p == '(') {
                CONST char *q = p;
                do {
		    q++;
		} while (*q != '\0');
		q--;
		if (*q == ')') { /* we have an array element */
		    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		            "procedure \"", procName,
		            "\" has formal parameter \"", fieldValues[0],
			    "\" that is an array element",
			    (char *) NULL);
		    ckfree((char *) fieldValues);
		    goto procError;
		}
	    } else if ((*p == ':') && (*(p+1) == ':')) {
		Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		        "procedure \"", procName,
		        "\" has formal parameter \"", fieldValues[0],
			"\" that is not a simple name",
			(char *) NULL);
		ckfree((char *) fieldValues);
		goto procError;
	    }
	    p++;
	}

	if (precompiled) {
	    /*
	     * Compare the parsed argument with the stored one.
	     * For the flags, we and out VAR_UNDEFINED to support bridging
	     * precompiled <= 8.3 code in 8.4 where this is now used as an
	     * optimization indicator.	Yes, this is a hack. -- hobbs
	     */

	    if ((localPtr->nameLength != nameLength)
		    || (strcmp(localPtr->name, fieldValues[0]))
		    || (localPtr->frameIndex != i)
		    || ((localPtr->flags & ~VAR_UNDEFINED)
			    != (VAR_SCALAR | VAR_ARGUMENT))
		    || ((localPtr->defValuePtr == NULL)
			    && (fieldCount == 2))
		    || ((localPtr->defValuePtr != NULL)
			    && (fieldCount != 2))) {
		char buf[80 + TCL_INTEGER_SPACE];
		sprintf(buf, "\": formal parameter %d is inconsistent with precompiled body",
			i);
		Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			"procedure \"", procName,
			buf, (char *) NULL);
		ckfree((char *) fieldValues);
		goto procError;
	    }

            /*
             * compare the default value if any
             */

            if (localPtr->defValuePtr != NULL) {
                int tmpLength;
                char *tmpPtr = Tcl_GetStringFromObj(localPtr->defValuePtr,
                        &tmpLength);
                if ((valueLength != tmpLength)
                        || (strncmp(fieldValues[1], tmpPtr,
                                (size_t) tmpLength))) {
                    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                            "procedure \"", procName,
                            "\": formal parameter \"",
                            fieldValues[0],
                            "\" has default value inconsistent with precompiled body",
                            (char *) NULL);
                    ckfree((char *) fieldValues);
                    goto procError;
                }
            }

            localPtr = localPtr->nextPtr;
        } else {
            /*
             * Allocate an entry in the runtime procedure frame's array of
             * local variables for the argument.
             */

            localPtr = (CompiledLocal *) ckalloc((unsigned)
                    (sizeof(CompiledLocal) - sizeof(localPtr->name)
                            + nameLength+1));
            if (procPtr->firstLocalPtr == NULL) {
                procPtr->firstLocalPtr = procPtr->lastLocalPtr = localPtr;
            } else {
                procPtr->lastLocalPtr->nextPtr = localPtr;
                procPtr->lastLocalPtr = localPtr;
            }
            localPtr->nextPtr = NULL;
            localPtr->nameLength = nameLength;
            localPtr->frameIndex = i;
            localPtr->flags = VAR_SCALAR | VAR_ARGUMENT;
            localPtr->resolveInfo = NULL;

            if (fieldCount == 2) {
                localPtr->defValuePtr =
		    Tcl_NewStringObj(fieldValues[1], valueLength);
                Tcl_IncrRefCount(localPtr->defValuePtr);
            } else {
                localPtr->defValuePtr = NULL;
            }
            memcpy(localPtr->name, fieldValues[0], nameLength + 1);
	}

        ckfree((char *) fieldValues);
    }

    /*
     * Now initialize the new procedure's cmdPtr field. This will be used
     * later when the procedure is called to determine what namespace the
     * procedure will run in. This will be different than the current
     * namespace if the proc was renamed into a different namespace.
     */

    *procPtrPtr = procPtr;
    ckfree((char *) argArray);
    return TCL_OK;

procError:
    if (precompiled) {
        procPtr->refCount--;
    } else {
        Tcl_DecrRefCount(bodyPtr);
        while (procPtr->firstLocalPtr != NULL) {
            localPtr = procPtr->firstLocalPtr;
            procPtr->firstLocalPtr = localPtr->nextPtr;

            defPtr = localPtr->defValuePtr;
            if (defPtr != NULL) {
                Tcl_DecrRefCount(defPtr);
            }

            ckfree((char *) localPtr);
        }
        ckfree((char *) procPtr);
    }
    if (argArray != NULL) {
	ckfree((char *) argArray);
    }
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetFrame --
 *
 *	Given a description of a procedure frame, such as the first
 *	argument to an "uplevel" or "upvar" command, locate the
 *	call frame for the appropriate level of procedure.
 *
 * Results:
 *	The return value is -1 if an error occurred in finding the frame
 *	(in this case an error message is left in the interp's result).
 *	1 is returned if string was either a number or a number preceded
 *	by "#" and it specified a valid frame.  0 is returned if string
 *	isn't one of the two things above (in this case, the lookup
 *	acts as if string were "1").  The variable pointed to by
 *	framePtrPtr is filled in with the address of the desired frame
 *	(unless an error occurs, in which case it isn't modified).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TclGetFrame(interp, string, framePtrPtr)
    Tcl_Interp *interp;		/* Interpreter in which to find frame. */
    CONST char *string;		/* String describing frame. */
    CallFrame **framePtrPtr;	/* Store pointer to frame here (or NULL
				 * if global frame indicated). */
{
    register Interp *iPtr = (Interp *) interp;
    int curLevel, level, result;
    CallFrame *framePtr;

    /*
     * Parse string to figure out which level number to go to.
     */

    result = 1;
    curLevel = (iPtr->varFramePtr == NULL) ? 0 : iPtr->varFramePtr->level;
    if (*string == '#') {
	if (Tcl_GetInt(interp, string+1, &level) != TCL_OK) {
	    return -1;
	}
	if (level < 0) {
	    levelError:
	    Tcl_AppendResult(interp, "bad level \"", string, "\"",
		    (char *) NULL);
	    return -1;
	}
    } else if (isdigit(UCHAR(*string))) { /* INTL: digit */
	if (Tcl_GetInt(interp, string, &level) != TCL_OK) {
	    return -1;
	}
	level = curLevel - level;
    } else {
	level = curLevel - 1;
	result = 0;
    }

    /*
     * Figure out which frame to use, and modify the interpreter so
     * its variables come from that frame.
     */

    if (level == 0) {
	framePtr = NULL;
    } else {
	for (framePtr = iPtr->varFramePtr; framePtr != NULL;
		framePtr = framePtr->callerVarPtr) {
	    if (framePtr->level == level) {
		break;
	    }
	}
	if (framePtr == NULL) {
	    goto levelError;
	}
    }
    *framePtrPtr = framePtr;
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UplevelObjCmd --
 *
 *	This object procedure is invoked to process the "uplevel" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl object result value.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_UplevelObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    register Interp *iPtr = (Interp *) interp;
    char *optLevel;
    int result;
    CallFrame *savedVarFramePtr, *framePtr;

    if (objc < 2) {
	uplevelSyntax:
	Tcl_WrongNumArgs(interp, 1, objv, "?level? command ?arg ...?");
	return TCL_ERROR;
    }

    /*
     * Find the level to use for executing the command.
     */

    optLevel = TclGetString(objv[1]);
    result = TclGetFrame(interp, optLevel, &framePtr);
    if (result == -1) {
	return TCL_ERROR;
    }
    objc -= (result+1);
    if (objc == 0) {
	goto uplevelSyntax;
    }
    objv += (result+1);

    /*
     * Modify the interpreter state to execute in the given frame.
     */

    savedVarFramePtr = iPtr->varFramePtr;
    iPtr->varFramePtr = framePtr;

    /*
     * Execute the residual arguments as a command.
     */

    if (objc == 1) {
#ifdef TCL_TIP280
	/* TIP #280. Make argument location available to eval'd script */
	CmdFrame* invoker = NULL;
	int word          = 0;
	TclArgumentGet (interp, objv[0], &invoker, &word);
	result = TclEvalObjEx(interp, objv[0], TCL_EVAL_DIRECT, invoker, word);
#else
	result = Tcl_EvalObjEx(interp, objv[0], TCL_EVAL_DIRECT);
#endif
    } else {
	/*
	 * More than one argument: concatenate them together with spaces
	 * between, then evaluate the result.  Tcl_EvalObjEx will delete
	 * the object when it decrements its refcount after eval'ing it.
	 */
	Tcl_Obj *objPtr;

	objPtr = Tcl_ConcatObj(objc, objv);
	result = Tcl_EvalObjEx(interp, objPtr, TCL_EVAL_DIRECT);
    }
    if (result == TCL_ERROR) {
	char msg[32 + TCL_INTEGER_SPACE];
	sprintf(msg, "\n    (\"uplevel\" body line %d)", interp->errorLine);
	Tcl_AddObjErrorInfo(interp, msg, -1);
    }

    /*
     * Restore the variable frame, and return.
     */

    iPtr->varFramePtr = savedVarFramePtr;
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TclFindProc --
 *
 *	Given the name of a procedure, return a pointer to the
 *	record describing the procedure. The procedure will be
 *	looked up using the usual rules: first in the current
 *	namespace and then in the global namespace.
 *
 * Results:
 *	NULL is returned if the name doesn't correspond to any
 *	procedure. Otherwise, the return value is a pointer to
 *	the procedure's record. If the name is found but refers
 *	to an imported command that points to a "real" procedure
 *	defined in another namespace, a pointer to that "real"
 *	procedure's structure is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Proc *
TclFindProc(iPtr, procName)
    Interp *iPtr;		/* Interpreter in which to look. */
    CONST char *procName;		/* Name of desired procedure. */
{
    Tcl_Command cmd;
    Tcl_Command origCmd;
    Command *cmdPtr;

    cmd = Tcl_FindCommand((Tcl_Interp *) iPtr, procName,
            (Tcl_Namespace *) NULL, /*flags*/ 0);
    if (cmd == (Tcl_Command) NULL) {
        return NULL;
    }
    cmdPtr = (Command *) cmd;

    origCmd = TclGetOriginalCommand(cmd);
    if (origCmd != NULL) {
	cmdPtr = (Command *) origCmd;
    }
    if (cmdPtr->proc != TclProcInterpProc) {
	return NULL;
    }
    return (Proc *) cmdPtr->clientData;
}

/*
 *----------------------------------------------------------------------
 *
 * TclIsProc --
 *
 *	Tells whether a command is a Tcl procedure or not.
 *
 * Results:
 *	If the given command is actually a Tcl procedure, the
 *	return value is the address of the record describing
 *	the procedure.  Otherwise the return value is 0.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Proc *
TclIsProc(cmdPtr)
    Command *cmdPtr;		/* Command to test. */
{
    Tcl_Command origCmd;

    origCmd = TclGetOriginalCommand((Tcl_Command) cmdPtr);
    if (origCmd != NULL) {
	cmdPtr = (Command *) origCmd;
    }
    if (cmdPtr->proc == TclProcInterpProc) {
	return (Proc *) cmdPtr->clientData;
    }
    return (Proc *) 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TclProcInterpProc --
 *
 *	When a Tcl procedure gets invoked with an argc/argv array of
 *	strings, this routine gets invoked to interpret the procedure.
 *
 * Results:
 *	A standard Tcl result value, usually TCL_OK.
 *
 * Side effects:
 *	Depends on the commands in the procedure.
 *
 *----------------------------------------------------------------------
 */

int
TclProcInterpProc(clientData, interp, argc, argv)
    ClientData clientData;	/* Record describing procedure to be
				 * interpreted. */
    Tcl_Interp *interp;		/* Interpreter in which procedure was
				 * invoked. */
    int argc;			/* Count of number of arguments to this
				 * procedure. */
    register CONST char **argv;	/* Argument values. */
{
    register Tcl_Obj *objPtr;
    register int i;
    int result;

    /*
     * This procedure generates an objv array for object arguments that hold
     * the argv strings. It starts out with stack-allocated space but uses
     * dynamically-allocated storage if needed.
     */

#define NUM_ARGS 20
    Tcl_Obj *(objStorage[NUM_ARGS]);
    register Tcl_Obj **objv = objStorage;

    /*
     * Create the object argument array "objv". Make sure objv is large
     * enough to hold the objc arguments plus 1 extra for the zero
     * end-of-objv word.
     */

    if ((argc + 1) > NUM_ARGS) {
	objv = (Tcl_Obj **)
	    ckalloc((unsigned)(argc + 1) * sizeof(Tcl_Obj *));
    }

    for (i = 0;  i < argc;  i++) {
	objv[i] = Tcl_NewStringObj(argv[i], -1);
	Tcl_IncrRefCount(objv[i]);
    }
    objv[argc] = 0;

    /*
     * Use TclObjInterpProc to actually interpret the procedure.
     */

    result = TclObjInterpProc(clientData, interp, argc, objv);

    /*
     * Move the interpreter's object result to the string result,
     * then reset the object result.
     */

    Tcl_SetResult(interp, TclGetString(Tcl_GetObjResult(interp)),
	    TCL_VOLATILE);

    /*
     * Decrement the ref counts on the objv elements since we are done
     * with them.
     */

    for (i = 0;  i < argc;  i++) {
	objPtr = objv[i];
	TclDecrRefCount(objPtr);
    }

    /*
     * Free the objv array if malloc'ed storage was used.
     */

    if (objv != objStorage) {
	ckfree((char *) objv);
    }
    return result;
#undef NUM_ARGS
}

/*
 *----------------------------------------------------------------------
 *
 * TclObjInterpProc --
 *
 *	When a Tcl procedure gets invoked during bytecode evaluation, this
 *	object-based routine gets invoked to interpret the procedure.
 *
 * Results:
 *	A standard Tcl object result value.
 *
 * Side effects:
 *	Depends on the commands in the procedure.
 *
 *----------------------------------------------------------------------
 */

int
TclObjInterpProc(clientData, interp, objc, objv)
    ClientData clientData; 	 /* Record describing procedure to be
				  * interpreted. */
    register Tcl_Interp *interp; /* Interpreter in which procedure was
				  * invoked. */
    int objc;			 /* Count of number of arguments to this
				  * procedure. */
    Tcl_Obj *CONST objv[];	 /* Argument value objects. */
{
    Interp *iPtr = (Interp *) interp;
    Proc *procPtr = (Proc *) clientData;
    Namespace *nsPtr = procPtr->cmdPtr->nsPtr;
    CallFrame frame;
    register CallFrame *framePtr = &frame;
    register Var *varPtr;
    register CompiledLocal *localPtr;
    char *procName;
    int nameLen, localCt, numArgs, argCt, i, result;

    /*
     * This procedure generates an array "compiledLocals" that holds the
     * storage for local variables. It starts out with stack-allocated space
     * but uses dynamically-allocated storage if needed.
     */

#define NUM_LOCALS 20
    Var localStorage[NUM_LOCALS];
    Var *compiledLocals = localStorage;

    /*
     * Get the procedure's name.
     */

    procName = Tcl_GetStringFromObj(objv[0], &nameLen);

    /*
     * If necessary, compile the procedure's body. The compiler will
     * allocate frame slots for the procedure's non-argument local
     * variables.  Note that compiling the body might increase
     * procPtr->numCompiledLocals if new local variables are found
     * while compiling.
     */

    result = ProcCompileProc(interp, procPtr, procPtr->bodyPtr, nsPtr,
	    "body of proc", procName, &procPtr);

    if (result != TCL_OK) {
        return result;
    }

    /*
     * Create the "compiledLocals" array. Make sure it is large enough to
     * hold all the procedure's compiled local variables, including its
     * formal parameters.
     */

    localCt = procPtr->numCompiledLocals;
    if (localCt > NUM_LOCALS) {
	compiledLocals = (Var *) ckalloc((unsigned) localCt * sizeof(Var));
    }

    /*
     * Set up and push a new call frame for the new procedure invocation.
     * This call frame will execute in the proc's namespace, which might
     * be different than the current namespace. The proc's namespace is
     * that of its command, which can change if the command is renamed
     * from one namespace to another.
     */

    result = Tcl_PushCallFrame(interp, (Tcl_CallFrame *) framePtr,
            (Tcl_Namespace *) nsPtr, /*isProcCallFrame*/ 1);

    if (result != TCL_OK) {
        return result;
    }

    framePtr->objc = objc;
    framePtr->objv = objv;  /* ref counts for args are incremented below */

    /*
     * Initialize and resolve compiled variable references.
     */

    framePtr->procPtr = procPtr;
    framePtr->numCompiledLocals = localCt;
    framePtr->compiledLocals = compiledLocals;

    TclInitCompiledLocals(interp, framePtr, nsPtr);

    /*
     * Match and assign the call's actual parameters to the procedure's
     * formal arguments. The formal arguments are described by the first
     * numArgs entries in both the Proc structure's local variable list and
     * the call frame's local variable array.
     */

    numArgs = procPtr->numArgs;
    varPtr = framePtr->compiledLocals;
    localPtr = procPtr->firstLocalPtr;
    argCt = objc;
    for (i = 1, argCt -= 1;  i <= numArgs;  i++, argCt--) {
	if (!TclIsVarArgument(localPtr)) {
	    panic("TclObjInterpProc: local variable %s is not argument but should be",
		  localPtr->name);
	    return TCL_ERROR;
	}
	if (TclIsVarTemporary(localPtr)) {
	    panic("TclObjInterpProc: local variable %d is temporary but should be an argument", i);
	    return TCL_ERROR;
	}

	/*
	 * Handle the special case of the last formal being "args".  When
	 * it occurs, assign it a list consisting of all the remaining
	 * actual arguments.
	 */

	if ((i == numArgs) && ((localPtr->name[0] == 'a')
	        && (strcmp(localPtr->name, "args") == 0))) {
	    Tcl_Obj *listPtr = Tcl_NewListObj(argCt, &(objv[i]));
	    varPtr->value.objPtr = listPtr;
	    Tcl_IncrRefCount(listPtr); /* local var is a reference */
	    TclClearVarUndefined(varPtr);
	    argCt = 0;
	    break;		/* done processing args */
	} else if (argCt > 0) {
	    Tcl_Obj *objPtr = objv[i];
	    varPtr->value.objPtr = objPtr;
	    TclClearVarUndefined(varPtr);
	    Tcl_IncrRefCount(objPtr);  /* since the local variable now has
					* another reference to object. */
	} else if (localPtr->defValuePtr != NULL) {
	    Tcl_Obj *objPtr = localPtr->defValuePtr;
	    varPtr->value.objPtr = objPtr;
	    TclClearVarUndefined(varPtr);
	    Tcl_IncrRefCount(objPtr);  /* since the local variable now has
					* another reference to object. */
	} else {
	    goto incorrectArgs;
	}
	varPtr++;
	localPtr = localPtr->nextPtr;
    }
    if (argCt > 0) {
	Tcl_Obj *objResult;
	int len, flags;

	incorrectArgs:
	/*
	 * Build up equivalent to Tcl_WrongNumArgs message for proc
	 */

	Tcl_ResetResult(interp);
	objResult = Tcl_GetObjResult(interp);
	Tcl_AppendToObj(objResult, "wrong # args: should be \"", -1);

	/*
	 * Quote the proc name if it contains spaces (Bug 942757).
	 */

	len = Tcl_ScanCountedElement(procName, nameLen, &flags);
	if (len != nameLen) {
	    char *procName1 = ckalloc((unsigned) len + 1);
	    len = Tcl_ConvertCountedElement(procName, nameLen, procName1, flags);
	    Tcl_AppendToObj(objResult, procName1, len);
	    ckfree(procName1);
	} else {
	    Tcl_AppendToObj(objResult, procName, len);
	}

	localPtr = procPtr->firstLocalPtr;
	for (i = 1;  i <= numArgs;  i++) {
	    if (localPtr->defValuePtr != NULL) {
		Tcl_AppendStringsToObj(objResult,
			" ?", localPtr->name, "?", (char *) NULL);
	    } else {
		Tcl_AppendStringsToObj(objResult,
			" ", localPtr->name, (char *) NULL);
	    }
	    localPtr = localPtr->nextPtr;
	}
	Tcl_AppendStringsToObj(objResult, "\"", (char *) NULL);

	result = TCL_ERROR;
	goto procDone;
    }

    /*
     * Invoke the commands in the procedure's body.
     */

#ifdef TCL_COMPILE_DEBUG
    if (tclTraceExec >= 1) {
	fprintf(stdout, "Calling proc ");
	for (i = 0;  i < objc;  i++) {
	    TclPrintObject(stdout, objv[i], 15);
	    fprintf(stdout, " ");
	}
	fprintf(stdout, "\n");
	fflush(stdout);
    }
#endif /*TCL_COMPILE_DEBUG*/

#ifdef USE_DTRACE
    if (TCL_DTRACE_PROC_ARGS_ENABLED()) {
	char *a[10];
	int i = 0;

	while (i < 10) {
	    a[i] = i < objc ? TclGetString(objv[i]) : NULL; i++;
	}
	TCL_DTRACE_PROC_ARGS(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
		a[8], a[9]);
    }
#endif /* USE_DTRACE */

    iPtr->returnCode = TCL_OK;
    procPtr->refCount++;
    if (TCL_DTRACE_PROC_ENTRY_ENABLED()) {
	TCL_DTRACE_PROC_ENTRY(TclGetString(objv[0]), objc - 1,
		(Tcl_Obj **)(objv + 1));
    }
#ifndef TCL_TIP280
    result = TclCompEvalObj(interp, procPtr->bodyPtr);
#else
    /* TIP #280: No need to set the invoking context here. The body has
     * already been compiled, so the part of CompEvalObj using it is bypassed.
     */

    result = TclCompEvalObj(interp, procPtr->bodyPtr, NULL, 0);
#endif
    if (TCL_DTRACE_PROC_RETURN_ENABLED()) {
	TCL_DTRACE_PROC_RETURN(TclGetString(objv[0]), result);
    }
    procPtr->refCount--;
    if (procPtr->refCount <= 0) {
	TclProcCleanupProc(procPtr);
    }

    if (result != TCL_OK) {
	result = ProcessProcResultCode(interp, procName, nameLen, result);
    }

#ifdef USE_DTRACE
    if (TCL_DTRACE_PROC_RESULT_ENABLED()) {
	Tcl_Obj *r;

	r = Tcl_GetObjResult(interp);
	TCL_DTRACE_PROC_RESULT(TclGetString(objv[0]), result,
		TclGetString(r), r);
    }
#endif /* USE_DTRACE */

    /*
     * Pop and free the call frame for this procedure invocation, then
     * free the compiledLocals array if malloc'ed storage was used.
     */

    procDone:
    Tcl_PopCallFrame(interp);
    if (compiledLocals != localStorage) {
	ckfree((char *) compiledLocals);
    }
    return result;
#undef NUM_LOCALS
}

/*
 *----------------------------------------------------------------------
 *
 * TclProcCompileProc --
 *
 *	Called just before a procedure is executed to compile the
 *	body to byte codes.  If the type of the body is not
 *	"byte code" or if the compile conditions have changed
 *	(namespace context, epoch counters, etc.) then the body
 *	is recompiled.  Otherwise, this procedure does nothing.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May change the internal representation of the body object
 *	to compiled code.
 *
 *----------------------------------------------------------------------
 */

int
TclProcCompileProc(interp, procPtr, bodyPtr, nsPtr, description, procName)
    Tcl_Interp *interp;		/* Interpreter containing procedure. */
    Proc *procPtr;		/* Data associated with procedure. */
    Tcl_Obj *bodyPtr;		/* Body of proc. (Usually procPtr->bodyPtr,
 				 * but could be any code fragment compiled
 				 * in the context of this procedure.) */
    Namespace *nsPtr;		/* Namespace containing procedure. */
    CONST char *description;	/* string describing this body of code. */
    CONST char *procName;	/* Name of this procedure. */
{
    return ProcCompileProc(interp, procPtr, bodyPtr, nsPtr,
	    description, procName, NULL);
}

static int
ProcCompileProc(interp, procPtr, bodyPtr, nsPtr, description,
		procName, procPtrPtr)
    Tcl_Interp *interp;		/* Interpreter containing procedure. */
    Proc *procPtr;		/* Data associated with procedure. */
    Tcl_Obj *bodyPtr;		/* Body of proc. (Usually procPtr->bodyPtr,
 				 * but could be any code fragment compiled
 				 * in the context of this procedure.) */
    Namespace *nsPtr;		/* Namespace containing procedure. */
    CONST char *description;	/* string describing this body of code. */
    CONST char *procName;	/* Name of this procedure. */
    Proc **procPtrPtr;		/* points to storage where a replacement
				 * (Proc *) value may be written, when
				 * appropriate */
{
    Interp *iPtr = (Interp*)interp;
    int i, result;
    Tcl_CallFrame frame;
    ByteCode *codePtr = (ByteCode *) bodyPtr->internalRep.otherValuePtr;
    CompiledLocal *localPtr;

    /*
     * If necessary, compile the procedure's body. The compiler will
     * allocate frame slots for the procedure's non-argument local
     * variables. If the ByteCode already exists, make sure it hasn't been
     * invalidated by someone redefining a core command (this might make the
     * compiled code wrong). Also, if the code was compiled in/for a
     * different interpreter, we recompile it. Note that compiling the body
     * might increase procPtr->numCompiledLocals if new local variables are
     * found while compiling.
     *
     * Precompiled procedure bodies, however, are immutable and therefore
     * they are not recompiled, even if things have changed.
     */

    if (bodyPtr->typePtr == &tclByteCodeType) {
 	if (((Interp *) *codePtr->interpHandle != iPtr)
 	        || (codePtr->compileEpoch != iPtr->compileEpoch)
 	        || (codePtr->nsPtr != nsPtr)) {
            if (codePtr->flags & TCL_BYTECODE_PRECOMPILED) {
                if ((Interp *) *codePtr->interpHandle != iPtr) {
                    Tcl_AppendResult(interp,
                            "a precompiled script jumped interps", NULL);
                    return TCL_ERROR;
                }
	        codePtr->compileEpoch = iPtr->compileEpoch;
                codePtr->nsPtr = nsPtr;
            } else {
                (*tclByteCodeType.freeIntRepProc)(bodyPtr);
                bodyPtr->typePtr = (Tcl_ObjType *) NULL;
            }
 	}
    }
    if (bodyPtr->typePtr != &tclByteCodeType) {
 	int numChars;
 	char *ellipsis;

#ifdef TCL_COMPILE_DEBUG
 	if (tclTraceCompile >= 1) {
 	    /*
 	     * Display a line summarizing the top level command we
 	     * are about to compile.
 	     */

 	    numChars = strlen(procName);
 	    ellipsis = "";
 	    if (numChars > 50) {
 		numChars = 50;
 		ellipsis = "...";
 	    }
 	    fprintf(stdout, "Compiling %s \"%.*s%s\"\n",
 		    description, numChars, procName, ellipsis);
 	}
#endif

 	/*
 	 * Plug the current procPtr into the interpreter and coerce
 	 * the code body to byte codes.  The interpreter needs to
 	 * know which proc it's compiling so that it can access its
 	 * list of compiled locals.
 	 *
 	 * TRICKY NOTE:  Be careful to push a call frame with the
 	 *   proper namespace context, so that the byte codes are
 	 *   compiled in the appropriate class context.
 	 */

	if (procPtrPtr != NULL && procPtr->refCount > 1) {
	    Tcl_Command token;
	    Tcl_CmdInfo info;
	    Proc *new = (Proc *) ckalloc(sizeof(Proc));

	    new->iPtr = procPtr->iPtr;
	    new->refCount = 1;
	    new->cmdPtr = procPtr->cmdPtr;
	    token = (Tcl_Command) new->cmdPtr;
	    new->bodyPtr = Tcl_DuplicateObj(bodyPtr);
	    bodyPtr = new->bodyPtr;
	    Tcl_IncrRefCount(bodyPtr);
	    new->numArgs = procPtr->numArgs;

	    new->numCompiledLocals = new->numArgs;
	    new->firstLocalPtr = NULL;
	    new->lastLocalPtr = NULL;
	    localPtr = procPtr->firstLocalPtr;
	    for (i = 0; i < new->numArgs; i++, localPtr = localPtr->nextPtr) {
		CompiledLocal *copy = (CompiledLocal *) ckalloc((unsigned)
			(sizeof(CompiledLocal) -sizeof(localPtr->name)
			 + localPtr->nameLength + 1));
		if (new->firstLocalPtr == NULL) {
		    new->firstLocalPtr = new->lastLocalPtr = copy;
		} else {
		    new->lastLocalPtr->nextPtr = copy;
		    new->lastLocalPtr = copy;
		}
		copy->nextPtr = NULL;
		copy->nameLength = localPtr->nameLength;
		copy->frameIndex = localPtr->frameIndex;
		copy->flags = localPtr->flags;
		copy->defValuePtr = localPtr->defValuePtr;
		if (copy->defValuePtr) {
		    Tcl_IncrRefCount(copy->defValuePtr);
		}
		copy->resolveInfo = localPtr->resolveInfo;
		memcpy(copy->name, localPtr->name,  localPtr->nameLength + 1);
	    }


	    /* Reset the ClientData */
	    Tcl_GetCommandInfoFromToken(token, &info);
	    if (info.objClientData == (ClientData) procPtr) {
	        info.objClientData = (ClientData) new;
	    }
	    if (info.clientData == (ClientData) procPtr) {
	        info.clientData = (ClientData) new;
	    }
	    if (info.deleteData == (ClientData) procPtr) {
	        info.deleteData = (ClientData) new;
	    }
	    Tcl_SetCommandInfoFromToken(token, &info);

	    procPtr->refCount--;
	    *procPtrPtr = procPtr = new;
	}
 	iPtr->compiledProcPtr = procPtr;

 	result = Tcl_PushCallFrame(interp, &frame,
		(Tcl_Namespace*)nsPtr, /* isProcCallFrame */ 0);

 	if (result == TCL_OK) {
#ifdef TCL_TIP280
	    /* TIP #280. We get the invoking context from the cmdFrame
	     * which was saved by 'Tcl_ProcObjCmd' (using linePBodyPtr).
	     */

	    Tcl_HashEntry* hePtr = Tcl_FindHashEntry (iPtr->linePBodyPtr, (char *) procPtr);

	    /* Constructed saved frame has body as word 0. See Tcl_ProcObjCmd.
	     */
	    iPtr->invokeWord        = 0;
	    iPtr->invokeCmdFramePtr = (hePtr
				       ? (CmdFrame*) Tcl_GetHashValue (hePtr)
				       : NULL);
#endif
	    result = tclByteCodeType.setFromAnyProc(interp, bodyPtr);
#ifdef TCL_TIP280
	    iPtr->invokeCmdFramePtr = NULL;
#endif
	    Tcl_PopCallFrame(interp);
	}

 	if (result != TCL_OK) {
 	    if (result == TCL_ERROR) {
		char buf[100 + TCL_INTEGER_SPACE];

		numChars = strlen(procName);
 		ellipsis = "";
 		if (numChars > 50) {
 		    numChars = 50;
 		    ellipsis = "...";
 		}
		while ( (procName[numChars] & 0xC0) == 0x80 ) {
	            /*
		     * Back up truncation point so that we don't truncate
		     * in the middle of a multi-byte character (in UTF-8)
		     */
		    numChars--;
		    ellipsis = "...";
		}
 		sprintf(buf, "\n    (compiling %s \"%.*s%s\", line %d)",
 			description, numChars, procName, ellipsis,
 			interp->errorLine);
 		Tcl_AddObjErrorInfo(interp, buf, -1);
 	    }
 	    return result;
 	}
    } else if (codePtr->nsEpoch != nsPtr->resolverEpoch) {

	/*
	 * The resolver epoch has changed, but we only need to invalidate
	 * the resolver cache.
	 */

	for (localPtr = procPtr->firstLocalPtr;  localPtr != NULL;
	    localPtr = localPtr->nextPtr) {
	    localPtr->flags &= ~(VAR_RESOLVED);
	    if (localPtr->resolveInfo) {
		if (localPtr->resolveInfo->deleteProc) {
		    localPtr->resolveInfo->deleteProc(localPtr->resolveInfo);
		} else {
		    ckfree((char*)localPtr->resolveInfo);
		}
		localPtr->resolveInfo = NULL;
	    }
	}
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ProcessProcResultCode --
 *
 *	Procedure called by TclObjInterpProc to process a return code other
 *	than TCL_OK returned by a Tcl procedure.
 *
 * Results:
 *	Depending on the argument return code, the result returned is
 *	another return code and the interpreter's result is set to a value
 *	to supplement that return code.
 *
 * Side effects:
 *	If the result returned is TCL_ERROR, traceback information about
 *	the procedure just executed is appended to the interpreter's
 *	"errorInfo" variable.
 *
 *----------------------------------------------------------------------
 */

static int
ProcessProcResultCode(interp, procName, nameLen, returnCode)
    Tcl_Interp *interp;		/* The interpreter in which the procedure
				 * was called and returned returnCode. */
    char *procName;		/* Name of the procedure. Used for error
				 * messages and trace information. */
    int nameLen;		/* Number of bytes in procedure's name. */
    int returnCode;		/* The unexpected result code. */
{
    Interp *iPtr = (Interp *) interp;
    char msg[100 + TCL_INTEGER_SPACE];
    char *ellipsis = "";

    if (returnCode == TCL_OK) {
	return TCL_OK;
    }
    if ((returnCode > TCL_CONTINUE) || (returnCode < TCL_OK)) {
	return returnCode;
    }
    if (returnCode == TCL_RETURN) {
	return TclUpdateReturnInfo(iPtr);
    }
    if (returnCode != TCL_ERROR) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp), ((returnCode == TCL_BREAK)
		? "invoked \"break\" outside of a loop"
		: "invoked \"continue\" outside of a loop"), -1);
    }
    if (nameLen > 60) {
	nameLen = 60;
	ellipsis = "...";
    }
    while ( (procName[nameLen] & 0xC0) == 0x80 ) {
        /*
	 * Back up truncation point so that we don't truncate in the
	 * middle of a multi-byte character (in UTF-8)
	 */
	nameLen--;
	ellipsis = "...";
    }
    sprintf(msg, "\n    (procedure \"%.*s%s\" line %d)", nameLen, procName,
	    ellipsis, iPtr->errorLine);
    Tcl_AddObjErrorInfo(interp, msg, -1);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TclProcDeleteProc --
 *
 *	This procedure is invoked just before a command procedure is
 *	removed from an interpreter.  Its job is to release all the
 *	resources allocated to the procedure.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory gets freed, unless the procedure is actively being
 *	executed.  In this case the cleanup is delayed until the
 *	last call to the current procedure completes.
 *
 *----------------------------------------------------------------------
 */

void
TclProcDeleteProc(clientData)
    ClientData clientData;		/* Procedure to be deleted. */
{
    Proc *procPtr = (Proc *) clientData;

    procPtr->refCount--;
    if (procPtr->refCount <= 0) {
	TclProcCleanupProc(procPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclProcCleanupProc --
 *
 *	This procedure does all the real work of freeing up a Proc
 *	structure.  It's called only when the structure's reference
 *	count becomes zero.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory gets freed.
 *
 *----------------------------------------------------------------------
 */

void
TclProcCleanupProc(procPtr)
    register Proc *procPtr;		/* Procedure to be deleted. */
{
    register CompiledLocal *localPtr;
    Tcl_Obj *bodyPtr = procPtr->bodyPtr;
    Tcl_Obj *defPtr;
    Tcl_ResolvedVarInfo *resVarInfo;
#ifdef TCL_TIP280
    Tcl_HashEntry* hePtr = NULL;
    CmdFrame*      cfPtr = NULL;
    Interp*        iPtr  = procPtr->iPtr;
#endif

    if (bodyPtr != NULL) {
	Tcl_DecrRefCount(bodyPtr);
    }
    for (localPtr = procPtr->firstLocalPtr;  localPtr != NULL;  ) {
	CompiledLocal *nextPtr = localPtr->nextPtr;

        resVarInfo = localPtr->resolveInfo;
	if (resVarInfo) {
	    if (resVarInfo->deleteProc) {
		(*resVarInfo->deleteProc)(resVarInfo);
	    } else {
		ckfree((char *) resVarInfo);
	    }
        }

	if (localPtr->defValuePtr != NULL) {
	    defPtr = localPtr->defValuePtr;
	    Tcl_DecrRefCount(defPtr);
	}
	ckfree((char *) localPtr);
	localPtr = nextPtr;
    }
    ckfree((char *) procPtr);

#ifdef TCL_TIP280
    /* TIP #280. Release the location data associated with this Proc
     * structure, if any. The interpreter may not exist (For example for
     * procbody structurues created by tbcload.
     */

    if (!iPtr) return;

    hePtr = Tcl_FindHashEntry (iPtr->linePBodyPtr, (char *) procPtr);
    if (!hePtr) return;

    cfPtr = (CmdFrame*) Tcl_GetHashValue (hePtr);

    if (cfPtr->type == TCL_LOCATION_SOURCE) {
        Tcl_DecrRefCount (cfPtr->data.eval.path);
	cfPtr->data.eval.path = NULL;
    }
    ckfree ((char*) cfPtr->line); cfPtr->line = NULL;
    ckfree ((char*) cfPtr);
    Tcl_DeleteHashEntry (hePtr);
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * TclUpdateReturnInfo --
 *
 *	This procedure is called when procedures return, and at other
 *	points where the TCL_RETURN code is used.  It examines fields
 *	such as iPtr->returnCode and iPtr->errorCode and modifies
 *	the real return status accordingly.
 *
 * Results:
 *	The return value is the true completion code to use for
 *	the procedure, instead of TCL_RETURN.
 *
 * Side effects:
 *	The errorInfo and errorCode variables may get modified.
 *
 *----------------------------------------------------------------------
 */

int
TclUpdateReturnInfo(iPtr)
    Interp *iPtr;		/* Interpreter for which TCL_RETURN
				 * exception is being processed. */
{
    int code;
    char *errorCode;
    Tcl_Obj *objPtr;

    code = iPtr->returnCode;
    iPtr->returnCode = TCL_OK;
    if (code == TCL_ERROR) {
	errorCode = ((iPtr->errorCode != NULL) ? iPtr->errorCode : "NONE");
	objPtr = Tcl_NewStringObj(errorCode, -1);
	Tcl_IncrRefCount(objPtr);
	Tcl_ObjSetVar2((Tcl_Interp *) iPtr, iPtr->execEnvPtr->errorCode,
	        NULL, objPtr, TCL_GLOBAL_ONLY);
	Tcl_DecrRefCount(objPtr);
	iPtr->flags |= ERROR_CODE_SET;
	if (iPtr->errorInfo != NULL) {
	    objPtr = Tcl_NewStringObj(iPtr->errorInfo, -1);
	    Tcl_IncrRefCount(objPtr);
	    Tcl_ObjSetVar2((Tcl_Interp *) iPtr, iPtr->execEnvPtr->errorInfo,
		    NULL, objPtr, TCL_GLOBAL_ONLY);
	    Tcl_DecrRefCount(objPtr);
	    iPtr->flags |= ERR_IN_PROGRESS;
	}
    }
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetInterpProc --
 *
 *  Returns a pointer to the TclProcInterpProc procedure; this is different
 *  from the value obtained from the TclProcInterpProc reference on systems
 *  like Windows where import and export versions of a procedure exported
 *  by a DLL exist.
 *
 * Results:
 *  Returns the internal address of the TclProcInterpProc procedure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

TclCmdProcType
TclGetInterpProc()
{
    return (TclCmdProcType) TclProcInterpProc;
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetObjInterpProc --
 *
 *  Returns a pointer to the TclObjInterpProc procedure; this is different
 *  from the value obtained from the TclObjInterpProc reference on systems
 *  like Windows where import and export versions of a procedure exported
 *  by a DLL exist.
 *
 * Results:
 *  Returns the internal address of the TclObjInterpProc procedure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

TclObjCmdProcType
TclGetObjInterpProc()
{
    return (TclObjCmdProcType) TclObjInterpProc;
}

/*
 *----------------------------------------------------------------------
 *
 * TclNewProcBodyObj --
 *
 *  Creates a new object, of type "procbody", whose internal
 *  representation is the given Proc struct.
 *  The newly created object's reference count is 0.
 *
 * Results:
 *  Returns a pointer to a newly allocated Tcl_Obj, 0 on error.
 *
 * Side effects:
 *  The reference count in the ByteCode attached to the Proc is bumped up
 *  by one, since the internal rep stores a pointer to it.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclNewProcBodyObj(procPtr)
    Proc *procPtr;	/* the Proc struct to store as the internal
                         * representation. */
{
    Tcl_Obj *objPtr;

    if (!procPtr) {
        return (Tcl_Obj *) NULL;
    }

    objPtr = Tcl_NewStringObj("", 0);

    if (objPtr) {
        objPtr->typePtr = &tclProcBodyType;
        objPtr->internalRep.otherValuePtr = (VOID *) procPtr;

        procPtr->refCount++;
    }

    return objPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * ProcBodyDup --
 *
 *  Tcl_ObjType's Dup function for the proc body object.
 *  Bumps the reference count on the Proc stored in the internal
 *  representation.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Sets up the object in dupPtr to be a duplicate of the one in srcPtr.
 *
 *----------------------------------------------------------------------
 */

static void ProcBodyDup(srcPtr, dupPtr)
    Tcl_Obj *srcPtr;		/* object to copy */
    Tcl_Obj *dupPtr;		/* target object for the duplication */
{
    Proc *procPtr = (Proc *) srcPtr->internalRep.otherValuePtr;

    dupPtr->typePtr = &tclProcBodyType;
    dupPtr->internalRep.otherValuePtr = (VOID *) procPtr;
    procPtr->refCount++;
}

/*
 *----------------------------------------------------------------------
 *
 * ProcBodyFree --
 *
 *  Tcl_ObjType's Free function for the proc body object.
 *  The reference count on its Proc struct is decreased by 1; if the count
 *  reaches 0, the proc is freed.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  If the reference count on the Proc struct reaches 0, the struct is freed.
 *
 *----------------------------------------------------------------------
 */

static void
ProcBodyFree(objPtr)
    Tcl_Obj *objPtr;		/* the object to clean up */
{
    Proc *procPtr = (Proc *) objPtr->internalRep.otherValuePtr;
    procPtr->refCount--;
    if (procPtr->refCount <= 0) {
        TclProcCleanupProc(procPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ProcBodySetFromAny --
 *
 *  Tcl_ObjType's SetFromAny function for the proc body object.
 *  Calls panic.
 *
 * Results:
 *  Theoretically returns a TCL result code.
 *
 * Side effects:
 *  Calls panic, since we can't set the value of the object from a string
 *  representation (or any other internal ones).
 *
 *----------------------------------------------------------------------
 */

static int
ProcBodySetFromAny(interp, objPtr)
    Tcl_Interp *interp;			/* current interpreter */
    Tcl_Obj *objPtr;			/* object pointer */
{
    panic("called ProcBodySetFromAny");

    /*
     * this to keep compilers happy.
     */

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ProcBodyUpdateString --
 *
 *  Tcl_ObjType's UpdateString function for the proc body object.
 *  Calls panic.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Calls panic, since we this type has no string representation.
 *
 *----------------------------------------------------------------------
 */

static void
ProcBodyUpdateString(objPtr)
    Tcl_Obj *objPtr;		/* the object to update */
{
    panic("called ProcBodyUpdateString");
}


/*
 *----------------------------------------------------------------------
 *
 * TclCompileNoOp --
 *
 *	Procedure called to compile noOp's
 *
 * Results:
 *	The return value is TCL_OK, indicating successful compilation.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute a noOp at runtime.
 *
 *----------------------------------------------------------------------
 */

static int
TclCompileNoOp(interp, parsePtr, envPtr)
    Tcl_Interp *interp;         /* Used for error reporting. */
    Tcl_Parse *parsePtr;        /* Points to a parse structure for the
                                 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;         /* Holds resulting instructions. */
{
    Tcl_Token *tokenPtr;
    int i, code;
    int savedStackDepth = envPtr->currStackDepth;

    tokenPtr = parsePtr->tokenPtr;
    for(i = 1; i < parsePtr->numWords; i++) {
	tokenPtr = tokenPtr + tokenPtr->numComponents + 1;
	envPtr->currStackDepth = savedStackDepth;

	if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    code = TclCompileTokens(interp, tokenPtr+1,
	            tokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		return code;
	    }
	    TclEmitOpcode(INST_POP, envPtr);
	}
    }
    envPtr->currStackDepth = savedStackDepth;
    TclEmitPush(TclRegisterLiteral(envPtr, "", 0, /*onHeap*/ 0), envPtr);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
