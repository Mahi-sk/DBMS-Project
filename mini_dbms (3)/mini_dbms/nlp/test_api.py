"""Quick standalone check that GEMINI_API_KEY is set and working.
Run: python test_api.py
"""
from dotenv import load_dotenv
load_dotenv()

import llm_fallback

if not llm_fallback.is_available():
    print("NOT CONFIGURED: GEMINI_API_KEY is missing or empty in nlp/.env")
else:
    print("Key found, calling the Gemini API...")
    try:
        sql = llm_fallback.translate_with_llm(
            "create a table called pets with an id and a nickname",
            schemas={},
        )
        if sql:
            print("SUCCESS. Model returned:")
            print("  " + sql)
        else:
            print("API call succeeded but the model returned no usable SQL "
                  "(returned UNSUPPORTED or empty). That's a prompt issue, "
                  "not a connectivity issue.")
    except RuntimeError as e:
        print("API CALL FAILED:", e)
