std::string GraphMerger::mergeComment(
    const std::string& curatedComment,
    const std::string& generatedComment
) {
    // If curated comment is empty, use the generated one
    if (curatedComment.empty()) {
        return generatedComment;
    }
    // Otherwise, keep the curated comment (it's authoritative)
    return curatedComment;
}
