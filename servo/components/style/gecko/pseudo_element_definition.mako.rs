/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/// Gecko's pseudo-element definition.
///
/// We intentionally double-box legacy ::-moz-tree pseudo-elements to keep the
/// size of PseudoElement (and thus selector components) small.
#[derive(Clone, Debug, Eq, Hash, MallocSizeOf, PartialEq, ToShmem)]
pub enum PseudoElement {
    % for pseudo in PSEUDOS:
        /// ${pseudo.value}
        % if pseudo.is_tree_pseudo_element():
        ${pseudo.capitalized_pseudo()}(thin_vec::ThinVec<Atom>),
        % elif pseudo.pseudo_ident == "highlight":
        ${pseudo.capitalized_pseudo()}(AtomIdent),
        % elif pseudo.is_named_view_transition_pseudo():
        ${pseudo.capitalized_pseudo()}(PtNameAndClassSelector),
        % else:
        ${pseudo.capitalized_pseudo()},
        % endif
    % endfor
    /// ::-webkit-* that we don't recognize
    /// https://github.com/whatwg/compat/issues/103
    UnknownWebkit(Atom),
}

/// Important: If you change this, you should also update Gecko's
/// nsCSSPseudoElements::IsEagerlyCascadedInServo.
<% EAGER_PSEUDOS = ["Before", "After", "FirstLine", "FirstLetter"] %>
<% TREE_PSEUDOS = [pseudo for pseudo in PSEUDOS if pseudo.is_tree_pseudo_element()] %>
<% NAMED_VT_PSEUDOS = [pseudo for pseudo in PSEUDOS if pseudo.is_named_view_transition_pseudo()] %>
<% SIMPLE_PSEUDOS = [pseudo for pseudo in PSEUDOS if pseudo.is_simple_pseudo_element()] %>

/// The number of eager pseudo-elements.
pub const EAGER_PSEUDO_COUNT: usize = ${len(EAGER_PSEUDOS)};

/// The number of non-functional pseudo-elements.
pub const SIMPLE_PSEUDO_COUNT: usize = ${len(SIMPLE_PSEUDOS)};

/// The number of tree pseudo-elements.
pub const TREE_PSEUDO_COUNT: usize = ${len(TREE_PSEUDOS)};

/// The number of all pseudo-elements.
pub const PSEUDO_COUNT: usize = ${len(PSEUDOS)};

/// The list of eager pseudos.
pub const EAGER_PSEUDOS: [PseudoElement; EAGER_PSEUDO_COUNT] = [
    % for eager_pseudo_name in EAGER_PSEUDOS:
    PseudoElement::${eager_pseudo_name},
    % endfor
];

<%def name="pseudo_element_variant(pseudo, tree_arg='..')">\
PseudoElement::${pseudo.capitalized_pseudo()}${"({})".format(tree_arg) if not pseudo.is_simple_pseudo_element() else ""}\
</%def>

impl PseudoElement {
    /// Returns an index of the pseudo-element.
    #[inline]
    pub fn index(&self) -> usize {
        match *self {
            % for i, pseudo in enumerate(PSEUDOS):
            ${pseudo_element_variant(pseudo)} => ${i},
            % endfor
            PseudoElement::UnknownWebkit(..) => unreachable!(),
        }
    }

    /// Returns an array of `None` values.
    ///
    /// FIXME(emilio): Integer generics can't come soon enough.
    pub fn pseudo_none_array<T>() -> [Option<T>; PSEUDO_COUNT] {
        [
            ${",\n            ".join(["None" for pseudo in PSEUDOS])}
        ]
    }

    /// Whether this pseudo-element is an anonymous box.
    #[inline]
    pub fn is_anon_box(&self) -> bool {
        match *self {
            % for pseudo in PSEUDOS:
                % if pseudo.is_anon_box():
                    ${pseudo_element_variant(pseudo)} => true,
                % endif
            % endfor
            _ => false,
        }
    }

    /// Whether this pseudo-element is eagerly-cascaded.
    #[inline]
    pub fn is_eager(&self) -> bool {
        matches!(*self,
                 ${" | ".join(map(lambda name: "PseudoElement::{}".format(name), EAGER_PSEUDOS))})
    }

    /// Whether this pseudo-element is tree pseudo-element.
    #[inline]
    pub fn is_tree_pseudo_element(&self) -> bool {
        match *self {
            % for pseudo in TREE_PSEUDOS:
            ${pseudo_element_variant(pseudo)} => true,
            % endfor
            _ => false,
        }
    }

    /// Whether this pseudo-element is a named view transition pseudo-element.
    #[inline]
    pub fn is_named_view_transition_pseudo_element(&self) -> bool {
        match *self {
            % for pseudo in NAMED_VT_PSEUDOS:
            ${pseudo_element_variant(pseudo)} => true,
            % endfor
            _ => false,
        }
    }

    /// Whether this pseudo-element is an unknown Webkit-prefixed pseudo-element.
    #[inline]
    pub fn is_unknown_webkit_pseudo_element(&self) -> bool {
        matches!(*self, PseudoElement::UnknownWebkit(..))
    }

    /// Gets the flags associated to this pseudo-element, or 0 if it's an
    /// anonymous box.
    pub fn flags(&self) -> u32 {
        match *self {
            % for pseudo in PSEUDOS:
                ${pseudo_element_variant(pseudo)} =>
                % if pseudo.is_tree_pseudo_element():
                    structs::CSS_PSEUDO_ELEMENT_ENABLED_IN_UA_SHEETS_AND_CHROME,
                % elif pseudo.is_anon_box():
                    structs::CSS_PSEUDO_ELEMENT_ENABLED_IN_UA_SHEETS,
                % else:
                    structs::SERVO_CSS_PSEUDO_ELEMENT_FLAGS_${pseudo.pseudo_ident},
                % endif
            % endfor
            PseudoElement::UnknownWebkit(..) => 0,
        }
    }

    /// Construct a pseudo-element from a `PseudoStyleType`.
    #[inline]
    pub fn from_pseudo_type(type_: PseudoStyleType, functional_pseudo_parameter: Option<AtomIdent>) -> Option<Self> {
        match type_ {
            % for pseudo in PSEUDOS:
            % if pseudo.is_simple_pseudo_element():
                PseudoStyleType::${pseudo.pseudo_ident} => {
                    debug_assert!(functional_pseudo_parameter.is_none());
                    Some(${pseudo_element_variant(pseudo)})
                },
            % elif pseudo.is_named_view_transition_pseudo():
                PseudoStyleType::${pseudo.pseudo_ident} => functional_pseudo_parameter.map(|p| {
                    PseudoElement::${pseudo.capitalized_pseudo()}(PtNameAndClassSelector::from_name(p.0))
                }),
            % endif
            % endfor
            PseudoStyleType::highlight => {
                match functional_pseudo_parameter {
                    Some(p) => Some(PseudoElement::Highlight(p)),
                    None => None
                }
            }
            _ => None,
        }
    }

    /// Construct a `PseudoStyleType` from a pseudo-element
    // FIXME: we probably have to return the arguments of -moz-tree. However, they are multiple
    // names, so we skip them for now (until we really need them).
    #[inline]
    pub fn pseudo_type_and_argument(&self) -> (PseudoStyleType, Option<&Atom>) {
        match *self {
            % for pseudo in PSEUDOS:
            % if pseudo.is_tree_pseudo_element():
                PseudoElement::${pseudo.capitalized_pseudo()}(..) => (PseudoStyleType::XULTree, None),
            % elif pseudo.pseudo_ident == "highlight":
                PseudoElement::${pseudo.capitalized_pseudo()}(ref value) => (PseudoStyleType::${pseudo.pseudo_ident}, Some(&value.0)),
            % elif pseudo.is_named_view_transition_pseudo():
                PseudoElement::${pseudo.capitalized_pseudo()}(ref value) => (PseudoStyleType::${pseudo.pseudo_ident}, Some(value.name())),
            % else:
                PseudoElement::${pseudo.capitalized_pseudo()} => (PseudoStyleType::${pseudo.pseudo_ident}, None),
            % endif
            % endfor
            PseudoElement::UnknownWebkit(..) => unreachable!(),
        }
    }

    /// Get the argument list of a tree pseudo-element.
    #[inline]
    pub fn tree_pseudo_args(&self) -> Option<&[Atom]> {
        match *self {
            % for pseudo in TREE_PSEUDOS:
            PseudoElement::${pseudo.capitalized_pseudo()}(ref args) => Some(args),
            % endfor
            _ => None,
        }
    }

    /// Construct a tree pseudo-element from atom and args.
    #[inline]
    pub fn from_tree_pseudo_atom(atom: &Atom, args: Box<[Atom]>) -> Option<Self> {
        % for pseudo in PSEUDOS:
        % if pseudo.is_tree_pseudo_element():
            if atom == &atom!("${pseudo.value}") {
                return Some(PseudoElement::${pseudo.capitalized_pseudo()}(args.into()));
            }
        % endif
        % endfor
        None
    }

    /// Constructs a pseudo-element from a string of text.
    ///
    /// Returns `None` if the pseudo-element is not recognised.
    #[inline]
    pub fn from_slice(name: &str, allow_unkown_webkit: bool) -> Option<Self> {
        // We don't need to support tree pseudos because functional
        // pseudo-elements needs arguments, and thus should be created
        // via other methods.
        ascii_case_insensitive_phf_map! {
            pseudo -> PseudoElement = {
                % for pseudo in SIMPLE_PSEUDOS:
                "${pseudo.value[1:]}" => ${pseudo_element_variant(pseudo)},
                % endfor
                // Alias some legacy prefixed pseudos to their standardized name at parse time:
                "-moz-selection" => PseudoElement::Selection,
                "-moz-placeholder" => PseudoElement::Placeholder,
                "-moz-list-bullet" => PseudoElement::Marker,
                "-moz-list-number" => PseudoElement::Marker,
            }
        }
        if let Some(p) = pseudo::get(name) {
            return Some(p.clone());
        }
        if starts_with_ignore_ascii_case(name, "-moz-tree-") {
            return PseudoElement::tree_pseudo_element(name, Default::default())
        }
        const WEBKIT_PREFIX: &str = "-webkit-";
        if allow_unkown_webkit && starts_with_ignore_ascii_case(name, WEBKIT_PREFIX) {
            let part = string_as_ascii_lowercase(&name[WEBKIT_PREFIX.len()..]);
            return Some(PseudoElement::UnknownWebkit(part.into()));
        }
        None
    }

    /// Constructs a tree pseudo-element from the given name and arguments.
    /// "name" must start with "-moz-tree-".
    ///
    /// Returns `None` if the pseudo-element is not recognized.
    #[inline]
    pub fn tree_pseudo_element(name: &str, args: thin_vec::ThinVec<Atom>) -> Option<Self> {
        debug_assert!(starts_with_ignore_ascii_case(name, "-moz-tree-"));
        let tree_part = &name[10..];
        % for pseudo in TREE_PSEUDOS:
            if tree_part.eq_ignore_ascii_case("${pseudo.value[11:]}") {
                return Some(${pseudo_element_variant(pseudo, "args")});
            }
        % endfor
        None
    }

    /// Returns true if this pseudo-element matches the given selector.
    pub fn matches(
        &self,
        pseudo_selector: &PseudoElement,
        element: &super::wrapper::GeckoElement,
    ) -> bool {
        if *self == *pseudo_selector {
            return true;
        }

        if std::mem::discriminant(self) != std::mem::discriminant(pseudo_selector) {
            return false;
        }

        // Check named view transition pseudo-elements.
        self.matches_named_view_transition_pseudo_element(pseudo_selector, element)
    }
}

impl ToCss for PseudoElement {
    fn to_css<W>(&self, dest: &mut W) -> fmt::Result where W: fmt::Write {
        dest.write_char(':')?;
        match *self {
            % for pseudo in (p for p in PSEUDOS if p.pseudo_ident != "highlight"):
            %if pseudo.is_named_view_transition_pseudo():
                PseudoElement::${pseudo.capitalized_pseudo()}(ref name_and_class) => {
                    dest.write_str("${pseudo.value}(")?;
                    name_and_class.to_css(dest)?;
                    dest.write_char(')')?;
                }
            %else:
                ${pseudo_element_variant(pseudo)} => dest.write_str("${pseudo.value}")?,
            %endif
            % endfor
            PseudoElement::Highlight(ref name) => {
                dest.write_str(":highlight(")?;
                serialize_atom_identifier(name, dest)?;
                dest.write_char(')')?;
            }
            PseudoElement::UnknownWebkit(ref atom) => {
                dest.write_str(":-webkit-")?;
                serialize_atom_identifier(atom, dest)?;
            }
        }
        if let Some(args) = self.tree_pseudo_args() {
            if !args.is_empty() {
                dest.write_char('(')?;
                let mut iter = args.iter();
                if let Some(first) = iter.next() {
                    serialize_atom_identifier(&first, dest)?;
                    for item in iter {
                        dest.write_str(", ")?;
                        serialize_atom_identifier(item, dest)?;
                    }
                }
                dest.write_char(')')?;
            }
        }
        Ok(())
    }
}
