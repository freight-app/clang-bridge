use crate::ffi;

/// LSP-compatible semantic token type constants.
pub mod tok_type {
    pub const NAMESPACE:   u8 = 0;
    pub const TYPE:        u8 = 1;
    pub const FUNCTION:    u8 = 2;
    pub const METHOD:      u8 = 3;
    pub const PROPERTY:    u8 = 4;
    pub const VARIABLE:    u8 = 5;
    pub const PARAMETER:   u8 = 6;
    pub const ENUM_MEMBER: u8 = 7;
    pub const MACRO:       u8 = 8;
}

/// A single classified token in the TU.
#[derive(Debug, Clone)]
pub struct SemanticToken {
    /// 1-based line.
    pub line: u32,
    /// 1-based column.
    pub col: u32,
    /// Name length in characters.
    pub length: u32,
    /// One of the `tok_type::*` constants.
    pub token_type: u8,
}

/// Owned list of semantic tokens returned by [`semantic_tokens`].
pub struct SemanticTokenList(*mut ffi::CB_SemanticTokenList);

impl SemanticTokenList {
    pub fn len(&self) -> usize {
        unsafe { ffi::cb_semantic_token_count(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, i: usize) -> SemanticToken {
        let mut raw = ffi::CB_SemanticToken { line: 0, col: 0, length: 0, token_type: 0 };
        unsafe { ffi::cb_semantic_token_get(self.0, i, &mut raw) };
        SemanticToken {
            line: raw.line,
            col: raw.col,
            length: raw.length,
            token_type: raw.token_type,
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = SemanticToken> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

impl Drop for SemanticTokenList {
    fn drop(&mut self) {
        unsafe { ffi::cb_semantic_token_list_destroy(self.0) }
    }
}

/// Classify every named identifier in the TU.
///
/// Results are sorted by `(line, col)`. Maps to LSP
/// `textDocument/semanticTokens/full`.
pub fn semantic_tokens(tu: &crate::TranslationUnit) -> SemanticTokenList {
    SemanticTokenList(unsafe { ffi::cb_semantic_tokens(tu.0) })
}
