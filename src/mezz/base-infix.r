REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "Infix operator symbol definitions"
    Rights: {
        Copyright 2012 REBOL Technologies
        Copyright 2012-2017 Rebol Open Source Contributors
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0.
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Purpose: {
        In R3-Alpha, an "OP!" function would gather its left argument greedily
        without waiting for further evaluation, and its right argument would
        stop processing if it hit another "OP!".  This meant that a sequence
        of all infix ops would appear to process left-to-right, e.g.
        `1 + 2 * 3` would be 9.
        
        Ren-C does not have an "OP!" function type, it just has FUNCTION!, but
        a WORD! can be SET with the /ENFIX refinement.  This indicates that
        when the function is dispatched through that word, it should get its
        first parameter from the left.  However it will obey the parameter
        conventions of the original function (including quoting).  Hence since
        ADD has normal parameter conventions, `+: enfix :add` would wind up
        with `1 + 2 * 3` as 7.

        So a new parameter convention indicated by ISSUE! is provided to get
        the "#tight" behavior of OP! arguments in R3-Alpha.
    }
]

; R3-Alpha has several forms illegal for SET-WORD! (e.g. `<:`)  Ren-C allows
; more of these things, but if they were top-level SET-WORD! in this file then
; R3-Alpha wouldn't be able to read it when used as bootstrap r3-make.  It
; also can't LOAD several WORD! forms that Ren-C can (e.g. `->`)
;
; So %b-init.c manually adds the keys via Add_Lib_Keys_R3Alpha_Cant_Make().
; R3-ALPHA-QUOTE annotates to warn not to try and assign SET-WORD! forms, and
; to bind interned strings.
;
r3-alpha-quote: func [:spelling [word! string!]] [
    either word? spelling [
        spelling
    ][
        bind (to word! spelling) (context of 'r3-alpha-quote)
    ]
]


; Make top-level words for things not added by %b-init.c (e.g. /, //)
;
+: -: *: **: and*: or+: xor+: _

for-each [math-op function-name] [
    +       add
    -       subtract
    *       multiply
    **      power

    /       divide ;-- !!! may become pathing operator (which also divides)

    //      remainder ;-- !!! bad WORD!... https://forum.rebol.info/t/275

    and*    and~
    or+     or~
    xor+    xor~
][
    ; Ren-C's infix math obeys the "tight" parameter convention of R3-Alpha.
    ; But since the prefix functions themselves have normal parameters, this
    ; would require a wrapping function...adding a level of inefficiency:
    ;
    ;     +: enfix func [#a #b] [add :a :b]
    ;
    ; TIGHTEN optimizes this by making a "re-skinned" version of the function
    ; with tight parameters, without adding extra overhead when called.  This
    ; mechanism will eventually generalized to do any rewriting of convention
    ; one wants (e.g. to switch one parameter from normal to quoted).
    ;
    set/enfix math-op (tighten get function-name)
]


; Make top-level words for things not added by %b-init.c
;
=: !=: ==: !==: =?: _

for-each [comparison-op function-name] [
    =       equal?
    <>      not-equal?
    <       lesser?
    <=      lesser-or-equal? ;-- !!! or left arrow?  Consider `=<`
    >       greater?
    >=      greater-or-equal?

    !=      not-equal? ;-- !!! http://www.rebol.net/r3blogs/0017.html 

    ==      strict-equal?
    !==     strict-not-equal?

    =?      same?
][
    ; !!! See discussion about the future of comparison operators:
    ; https://forum.rebol.info/t/349
    ;
    ; While they were "tight" in R3-Alpha, Ren-C makes them use normal
    ; parameters.  So you can write `if length of block = 10 + 20 [...]` and
    ; other expressive things.  It comes at the cost of making it so that
    ; `if not x = y [...]` is interpreted as `if (not x) = y [...]`, which
    ; all things considered is still pretty natural (and popular in many
    ; languages)...and a small price to pay.  Hence no TIGHTEN call here.
    ;
    set/enfix comparison-op (get function-name)
]


; !!! Originally in Rebol2 and R3-Alpha, ? was a synonym for HELP.  This seems
; wasteful for the language as a whole, when it's easy enough to type HELP,
; or add it to the console-specific abbreviations as H (as with Q for QUIT).
;
; This experiments with making `? var` equivalent to `set? 'var`.  Some are
; made uncomfortable by ? being prefix and not infix, but this is a very
; useful feature to have a shorthand for.  (Note: might `! var` being a
; shorthand for `not set? 'var` make more sense than meaning NOT, because
; there the tradeoff of literacy for symbology actually makes something a
; bit clearer instead of less clear?)
;
?: func [
    {Determine whether a word represents a variable that is SET?}

    'var [any-word! any-path!]
        {Variable name to test}
][
    ; Note: since this just changes the parameter convention, it could use a
    ; facade (the way TIGHTEN does) and run the native code for SET?.  Revisit
    ; when REDESCRIBE has this ability.
    ;
    set? var
]


; !!! Originally in Rebol2 and R3-Alpha, ?? was used to dump variables.  In
; the spirit of not wanting to take ? for something like HELP, that function
; has been defined as DUMP (and extended with significant new features).
;
; Instead, ?? is used to make an infix operator, that takes a condition on the
; left and a value on the right--like an IF that won't run blocks/functions.
; As a complement, !! is then taken as a parallel to ELSE, which will also not
; run blocks or functions.  This is a similar to these operators from Perl6:
;
; https://docs.perl6.org/language/operators#infix_??_!!
;
; However, note that if you say `1 < 2 ?? 3 + 3 !! 4 + 4`, both additions
; will be run.  To "block" evaluation, there has to be a BLOCK! somewhere,
; hence these are not meant as a generic substitute for IF and ELSE.
;
??: enfix func [
    {If left is true, return value on the right (as-is)}

    return: [<opt> any-value!]
        {Void if the condition is FALSEY?, else value}
    condition [any-value!]
    value [<opt> any-value!]
][
    if/only :condition [:value]
]

!!: enfix func [
    {If left isn't void, return it, else return value on the right (as-is)}

    return: [<opt> any-value!]
        {Left if it isn't void, else right}
    left [<opt> any-value!]
    right [<opt> any-value!]
][
    either-test-value/only :left [:right]
]


; THEN and ELSE are "non-TIGHTened" enfix functions which either pass through
; an argument or run a branch, based on void-ness of the argument.  They take
; advantage of the pattern of conditionals such as `if condition [...]` to
; only return void if the branch does not run, and never return void if it
; does run (void branch evaluations are forced to BLANK!)
;
; These could be implemented as specializations of the generic EITHER-TEST
; native.  But due to their common use they are hand-optimized into their own
; specialized natives: EITHER-TEST-VOID and EITHER-TEST-VALUE.

then: enfix redescribe [
    "Evaluate the branch if the left hand side expression is not void"
](
    comment [specialize 'either-test [test: :void?]]
    :either-test-void
)

then*: enfix redescribe [
    "Would be the same as THEN/ONLY, if infix functions dispatched from paths"
](
    specialize 'then [only: true]
)

else: enfix redescribe [
    "Evaluate the branch if the left hand side expression is void"
](
    comment [specialize 'either-test [test: :any-value?]]
    :either-test-value
)

else*: enfix redescribe [
    "Would be the same as ELSE/ONLY, if infix functions dispatched from paths"
](
    specialize 'else [only: true]
)

also-do: enfix :after ;-- temporarily %mezz-legacy.r defines ALSO as error


; SHORT-CIRCUIT BOOLEAN OPERATORS
;
; Traditionally Rebol didn't have the ability to "short circuit" expressions,
; because you could never find the end of an expression without running it.
; By means of the DON'T operation, Ren-C can (sometimes) find the end of an
; expression while disabling side-effects from it.  This is used to implement
; boolean short-circuit ops, as an alternative to ALL [...], ANY [...], etc.
;
; The way they work is that they are enfixed functions with one left argument,
; and a variadic right hand argument.  So they only have access to their
; left argument at first.  They examine that left argument and if there's no
; chance for the right hand side to change their answer, they DON'T it.
; Otherwise, they DO the right hand argument and examine it to get the final
; answer.

and: enfix func [
    {Short-circuit boolean AND}

    return: [logic!]
    left [any-value!]
        {Expression which will always be evaluated}
    right [any-value! <...>]
        {Expression will be evaluated if LEFT is TRUTHY?, skipped if FALSEY?}
][
    case [
        left [to-logic do/next right blank]
        don't/next right blank [false]
    ] else [
        fail [
            "Right hand of short-circuit AND must not be variadic."
            "Use ALL [...] instead, or put right-hand side in a GROUP! ()"
        ]
    ]
]

or: enfix func [
    {Short-circuit boolean AND}

    return: [logic!]
    left [any-value!]
        {Expression which will always be evaluated}
    right [any-value! <...>]
        {Expression will be evaluated if LEFT is FALSEY?, skipped if TRUTHY?}
][
    case [
        not left [to-logic do/next right blank]
        don't/next right blank [true]
    ] else [
        fail [
            "Right hand of short-circuit OR must not be variadic."
            "Use ANY [...] instead, or put right-hand side in a GROUP! ()"
        ]
    ]
]

nor: enfix func [
    {Short-circuit boolean NOR}

    return: [logic!]
    left [any-value!]
        {Expression which will always be evaluated}
    right [any-value! <...>]
        {Expression will be evaluated if LEFT is FALSEY?, skipped if TRUTHY?}
][
    case [
        not left [not do/next right blank]
        don't/next right blank [false]
    ] else [
        fail [
            "Right hand of short-circuit NOR must not be variadic."
            "Use NONE [...] instead, or put right-hand side in a GROUP! ()"
        ]
    ]
]

nand: enfix func [
    {Short-circuit boolean NAND}

    return: [logic!]
    left [any-value!]
        {Expression which will always be evaluated}
    right [any-value! <...>]
        {Expression will be evaluated if LEFT is FALSEY?, skipped if TRUTHY?}
][
    case [
        left [not do/next right blank]
        don't/next right blank [false]
    ] else [
        fail [
            "Right hand of short-circuit NAND must not be variadic."
            "Put right-hand side in a GROUP! ()"

            ;-- is there a good ANY/ALL/NONE-like parallel for NAND?
        ]
    ]
]


; There's no way to do a shortcut XOR...both sides have to be tested.
;
xor: enfix :xor?


; Lambdas are experimental quick function generators via a symbol
;
set/enfix (r3-alpha-quote "->") :lambda
set/enfix (r3-alpha-quote "<-") (specialize :lambda [only: true])


; These constructs used to be enfix to complete their left hand side.  Yet
; that form of completion was only one expression's worth, when they wanted
; to allow longer runs of evaluation.  "Invisible functions" (those which
; `return: []`) permit a more flexible version of the mechanic.

set (r3-alpha-quote "<|") :invisible-eval-all
set (r3-alpha-quote "|>") :right-bar
||: enfix :once-bar
