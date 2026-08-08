"""
Trains a small intent classifier that maps a natural-language sentence to
one of: CREATE_TABLE, INSERT, SELECT, DROP_TABLE.

Pipeline: TF-IDF (word 1-2 grams) -> Logistic Regression.
This is deliberately simple and fast (trains in well under a second) but is
a genuine, trained ML model -- not a lookup table -- so it generalizes to
phrasings it hasn't seen verbatim.

Run:
    python train_model.py
Produces:
    intent_model.joblib
"""

import joblib
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.linear_model import LogisticRegression
from sklearn.pipeline import Pipeline
from sklearn.model_selection import cross_val_score

from dataset import EXAMPLES


def main():
    texts = [t for t, _ in EXAMPLES]
    labels = [l for _, l in EXAMPLES]

    pipeline = Pipeline([
        ("tfidf", TfidfVectorizer(ngram_range=(1, 2), lowercase=True)),
        ("clf", LogisticRegression(max_iter=1000)),
    ])

    # Quick sanity check via cross-validation (small dataset, so this is
    # just a smoke test, not a rigorous benchmark).
    scores = cross_val_score(pipeline, texts, labels, cv=3)
    print(f"Cross-val accuracy: {scores.mean():.2f} (folds: {scores})")

    pipeline.fit(texts, labels)
    joblib.dump(pipeline, "intent_model.joblib")
    print("Saved intent_model.joblib")


if __name__ == "__main__":
    main()